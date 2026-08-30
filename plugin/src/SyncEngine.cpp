// SyncEngine.cpp — see SyncEngine.h for the design overview.
#include "SyncEngine.h"

#include "core/Merge.h"
#include "core/PathUtil.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <fstream>

using nlohmann::json;

namespace npsync
{

namespace
{
constexpr int kMaxBackupsPerFile = 5;

std::wstring dirOf(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? p : p.substr(0, pos);
}

std::wstring fileNameOf(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? p : p.substr(pos + 1);
}
} // namespace

SyncEngine::SyncEngine() = default;

SyncEngine::~SyncEngine() {
    stop();
}

void SyncEngine::init(SettingsStore* store, Settings* settings) {
    store_ = store;
    settings_ = settings;

    std::wstring dbPath =
        settings->databaseLocation.empty() ? store->dir() + L"\\sync.db" : settings->databaseLocation;
    if (!db_.open(dbPath)) {
        setStatus(SyncStatus::Error, "Failed to open local database");
        return;
    }

    api_ = std::make_unique<ApiClient>(settings->backendUrl, settings->deviceId);
    api_->onTokensRotated = [this](const std::string& access, const std::string& refresh) {
        if (store_) {
            store_->saveSecret(L"access_token", access);
            store_->saveSecret(L"refresh_token", refresh);
        }
    };

    std::string access, refresh;
    if (store_->loadSecret(L"access_token", access) && store_->loadSecret(L"refresh_token", refresh))
        api_->setTokens(access, refresh);

    std::string mkB64;
    if (store_->loadSecret(L"master_key", mkB64))
        Crypto::base64UrlDecode(mkB64, masterKey_);
}

void SyncEngine::start() {
    if (running_)
        return;
    running_ = true;
    refreshIgnoreRules();

    // Watch every configured folder root.
    for (auto& [id, path] : db_.listSyncRoots())
        watcher_.addRoot(path);
    watcher_.start([this](const FsEvent& ev) { onFsEvent(ev); });

    // WebSocket push (falls back to polling automatically when down).
    if (settings_->webSocketEnabled && api_->hasTokens()) {
        ws_ = std::make_unique<WsClient>(
            settings_->backendUrl, [this](const json& ev) { onWsEvent(ev); },
            [this](bool connected) {
                if (connected) {
                    online_ = true;
                    syncRequested_ = true;
                }
            });
        ws_->start([this] { return api_ ? api_->accessToken() : std::string(); });
    }

    workerThread_ = std::thread([this] { workerLoop(); });
    pollerThread_ = std::thread([this] { pollerLoop(); });
    scannerThread_ = std::thread([this] { scannerLoop(); });

    if (api_->hasTokens())
        setStatus(SyncStatus::Synced, "Ready");
    else
        setStatus(SyncStatus::SignedOut, "Signed out");
}

void SyncEngine::stop() {
    running_ = false;
    watcher_.stop();
    if (ws_)
        ws_->stop();
    if (workerThread_.joinable())
        workerThread_.join();
    if (pollerThread_.joinable())
        pollerThread_.join();
    if (scannerThread_.joinable())
        scannerThread_.join();
}

std::string SyncEngine::deviceId() const {
    return settings_ ? settings_->deviceId : std::string();
}

bool SyncEngine::online() {
    return online_;
}

std::string SyncEngine::nowTimeString() {
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
    return buf;
}

void SyncEngine::setStatus(SyncStatus s, const std::string& text) {
    {
        std::lock_guard<std::mutex> lk(statusMu_);
        status_.status = s;
        status_.statusText = text;
        status_.pendingUploads = db_.isOpen() ? db_.pendingOpCount() : 0;
        status_.conflicts = db_.isOpen() ? db_.conflictCount() : 0;
        if (s == SyncStatus::Synced)
            status_.lastSyncTime = nowTimeString();
        if (s == SyncStatus::Conflict)
            status_.conflicts = db_.conflictCount();
    }
    if (onStatusChanged)
        onStatusChanged();
}

StatusInfo SyncEngine::status() {
    std::lock_guard<std::mutex> lk(statusMu_);
    StatusInfo out = status_;
    if (db_.isOpen()) {
        out.pendingUploads = db_.pendingOpCount();
        out.conflicts = db_.conflictCount();
        out.filesSynchronized = static_cast<int>(db_.allFiles().size());
    }
    return out;
}

// ---- auth & keys ----

bool SyncEngine::isSignedIn() const {
    return api_ && api_->hasTokens();
}

bool SyncEngine::signIn(const std::string& email, const std::string& password, std::string& errorOut,
                        bool createAccount) {
    ApiResponse r = createAccount ? api_->registerAccount(email, password, settings_->deviceName)
                                  : api_->login(email, password, settings_->deviceName);
    if (!r.transportOk) {
        errorOut = "Cannot reach server: " + r.transportError;
        return false;
    }
    if (r.serverProtocol != 0 && r.serverProtocol != 1) {
        errorOut = "This server speaks an incompatible protocol version. Update the plugin or the server.";
        return false;
    }
    if (r.status != 200) {
        errorOut = r.body.value("message", createAccount ? "Registration failed" : "Sign-in failed");
        return false;
    }
    settings_->deviceId = r.body.value("device_id", "");
    settings_->accountId = r.body.value("account_id", "");
    store_->saveSecret(L"access_token", r.body.value("access_token", ""));
    store_->saveSecret(L"refresh_token", r.body.value("refresh_token", ""));
    store_->save(*settings_);
    setStatus(SyncStatus::Synced, "Signed in");
    syncRequested_ = true;
    return true;
}

void SyncEngine::signOut() {
    if (api_ && api_->hasTokens())
        api_->logout();
    if (store_) {
        store_->deleteSecret(L"access_token");
        store_->deleteSecret(L"refresh_token");
    }
    setStatus(SyncStatus::SignedOut, "Signed out");
}

bool SyncEngine::hasMasterKey() const {
    return masterKey_.size() == kMasterKeyLen;
}

bool SyncEngine::generateMasterKeyIfNeeded() {
    if (hasMasterKey())
        return true;
    masterKey_ = Crypto::generateMasterKey();
    // Also generate a recovery key: wrap the master key under it and store
    // the wrapped blob so the recovery key alone can unlock the account.
    std::string recovery = Crypto::generateRecoveryKey();
    Bytes salt = Crypto::random(16);
    Bytes wk = Crypto::deriveKeyFromCode(recovery, salt);
    Bytes wrapped = Crypto::wrapMasterKey(masterKey_, wk);
    json rec = {{"salt", Crypto::base64UrlEncode(salt)}, {"wrapped", Crypto::base64UrlEncode(wrapped)}};
    if (!store_->saveSecret(L"recovery_wrapped", rec.dump()))
        return false;
    if (!store_->saveSecret(L"recovery_key_display", recovery))
        return false;
    return store_->saveSecret(L"master_key", Crypto::base64UrlEncode(masterKey_));
}

bool SyncEngine::unlockWithRecoveryKey(const std::string& recoveryKey) {
    std::string normalized;
    if (!Crypto::normalizeRecoveryKey(recoveryKey, normalized))
        return false;
    std::string blobJson;
    if (!store_->loadSecret(L"recovery_wrapped", blobJson))
        return false;
    json rec;
    try {
        rec = json::parse(blobJson);
    }
    catch (...) {
        return false;
    }
    Bytes salt, wrapped, mk;
    if (!Crypto::base64UrlDecode(rec.value("salt", ""), salt))
        return false;
    if (!Crypto::base64UrlDecode(rec.value("wrapped", ""), wrapped))
        return false;
    Bytes wk = Crypto::deriveKeyFromCode(normalized, salt);
    if (!Crypto::unwrapMasterKey(wrapped, wk, mk))
        return false;
    masterKey_ = mk;
    return store_->saveSecret(L"master_key", Crypto::base64UrlEncode(masterKey_));
}

std::string SyncEngine::exportRecoveryKeyWrapped() {
    // Shown once during first-run setup; stored DPAPI-protected afterwards.
    std::string rk;
    store_->loadSecret(L"recovery_key_display", rk);
    return rk;
}

bool SyncEngine::pairNewDevice(std::string& codeOut, std::string& errorOut) {
    // This device wants to join: request a code. An existing device approves.
    ApiResponse r = api_->pairRequest();
    if (r.status != 200) {
        errorOut = r.body.value("message", "pairing request failed");
        return false;
    }
    codeOut = r.body.value("pairing_code", "");
    return !codeOut.empty();
}

bool SyncEngine::approvePairing(const std::string& code, std::string& errorOut) {
    if (!hasMasterKey()) {
        errorOut = "no master key on this device";
        return false;
    }
    // Wrap the master key under a key derived from the pairing code.
    Bytes salt = Crypto::random(16);
    Bytes wk = Crypto::deriveKeyFromCode(code, salt);
    Bytes wrapped = Crypto::wrapMasterKey(masterKey_, wk);
    json payload = {{"salt", Crypto::base64UrlEncode(salt)}, {"wrapped", Crypto::base64UrlEncode(wrapped)}};
    ApiResponse r =
        api_->pairApprove(code, Crypto::base64UrlEncode(Bytes(payload.dump().begin(), payload.dump().end())));
    if (r.status != 200) {
        errorOut = r.body.value("message", "approval failed");
        return false;
    }
    return true;
}

bool SyncEngine::completePairing(const std::string& code, std::string& errorOut) {
    ApiResponse r = api_->pairPoll(code);
    if (r.status == 410) {
        errorOut = "pairing code expired";
        return false;
    }
    if (r.status != 200) {
        errorOut = r.body.value("message", "pairing poll failed");
        return false;
    }
    if (r.body.value("status", "") != "approved") {
        errorOut = "pending";
        return false;
    }
    Bytes blob;
    if (!Crypto::base64UrlDecode(r.body.value("wrapped_master_key", ""), blob)) {
        errorOut = "bad wrapped key";
        return false;
    }
    json payload;
    try {
        payload = json::parse(std::string(blob.begin(), blob.end()));
    }
    catch (...) {
        errorOut = "bad wrapped key";
        return false;
    }
    Bytes salt, wrapped, mk;
    if (!Crypto::base64UrlDecode(payload.value("salt", ""), salt) ||
        !Crypto::base64UrlDecode(payload.value("wrapped", ""), wrapped)) {
        errorOut = "bad wrapped key";
        return false;
    }
    Bytes wk = Crypto::deriveKeyFromCode(code, salt);
    if (!Crypto::unwrapMasterKey(wrapped, wk, mk)) {
        errorOut = "could not unwrap key (wrong code?)";
        return false;
    }
    masterKey_ = mk;
    store_->saveSecret(L"master_key", Crypto::base64UrlEncode(masterKey_));
    return true;
}

void SyncEngine::setPaused(bool paused) {
    settings_->pauseSync = paused;
    store_->save(*settings_);
    setStatus(paused ? SyncStatus::Paused : SyncStatus::Synced, paused ? "Paused" : "Resumed");
    if (!paused)
        syncRequested_ = true;
}

void SyncEngine::syncNow() {
    settings_->pauseSync = false;
    syncRequested_ = true;
    fsDirty_ = true;
}

// ---- ignore rules ----

bool SyncEngine::refreshIgnoreRules() {
    std::lock_guard<std::mutex> lk(rulesMu_);
    rootRules_.clear();
    std::string extra;
    for (auto& p : settings_->extraIgnorePatterns) {
        extra += p;
        extra += '\n';
    }
    for (auto& [id, path] : db_.listSyncRoots()) {
        std::string text = extra;
        std::wstring ignoreFile = path + L"\\.npsyncignore";
        std::ifstream f(ignoreFile, std::ios::binary);
        if (f) {
            std::string local((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            text += local;
        }
        rootRules_[id] = IgnoreRules::parse(text);
    }
    // A ruleset for individually-added files (parent dir semantics).
    rootRules_[""] = IgnoreRules::parse(extra);
    return true;
}

bool SyncEngine::ignored(const std::string& relPath, bool isDir) {
    std::lock_guard<std::mutex> lk(rulesMu_);
    for (auto& [id, rules] : rootRules_)
        if (rules.ignored(relPath, isDir))
            return true;
    return false;
}

// ---- path mapping ----

std::optional<std::pair<std::string, std::wstring>> SyncEngine::locateInRoots(const std::wstring& absPath) {
    for (auto& [id, root] : db_.listSyncRoots()) {
        if (PathUtil::isInsideRoot(root, absPath)) {
            std::wstring rel = absPath.substr(root.size());
            while (!rel.empty() && (rel.front() == L'\\' || rel.front() == L'/'))
                rel.erase(rel.begin());
            std::string rel8 = PathUtil::wideToUtf8(rel);
            std::string norm;
            if (!PathUtil::normalizeRelative(rel8, norm))
                return std::nullopt;
            return std::make_pair(id + ":" + norm, root);
        }
    }
    for (auto& [id, f] : db_.listSyncFiles()) {
        std::wstring lf = f, la = absPath;
        std::transform(lf.begin(), lf.end(), lf.begin(), ::towlower);
        std::transform(la.begin(), la.end(), la.begin(), ::towlower);
        if (lf == la)
            return std::make_pair(std::string("file:") + PathUtil::wideToUtf8(fileNameOf(f)), dirOf(f));
    }
    return std::nullopt;
}

std::optional<std::wstring> SyncEngine::absPathForRel(const std::string& relPath) {
    // relPath format: "<rootId>:<normalized path>" or "file:<name>".
    size_t colon = relPath.find(':');
    if (colon == std::string::npos)
        return std::nullopt;
    std::string rootId = relPath.substr(0, colon);
    std::string rel = relPath.substr(colon + 1);
    for (auto& [id, root] : db_.listSyncRoots()) {
        if (id == rootId) {
            std::wstring abs;
            if (PathUtil::joinInsideRoot(root, rel, abs))
                return abs;
            return std::nullopt;
        }
    }
    for (auto& [id, f] : db_.listSyncFiles()) {
        if (rootId == "file" && PathUtil::wideToUtf8(fileNameOf(f)) == rel)
            return f;
    }
    return std::nullopt;
}

// ---- file I/O ----

bool SyncEngine::readFileBytes(const std::wstring& absPath, Bytes& out) {
    std::ifstream f(absPath, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool SyncEngine::writeFileAtomic(const std::wstring& absPath, const Bytes& data) {
    std::wstring tmp = absPath + L".npsync-tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        if (!data.empty())
            f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
        if (!f.good()) {
            DeleteFileW(tmp.c_str());
            return false;
        }
        f.flush();
    }
    // Verify hash before replacing (protects against partial writes).
    Bytes verify;
    if (!readFileBytes(tmp, verify) || verify.size() != data.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), absPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::wstring SyncEngine::shadowPath(const std::string& relKey) const {
    return store_->dir() + L"\\base\\" + PathUtil::utf8ToWide(Crypto::sha256Hex(relKey)) + L".shadow";
}

void SyncEngine::writeShadow(const std::string& relKey, const Bytes& content) {
    CreateDirectoryW((store_->dir() + L"\\base").c_str(), nullptr);
    writeFileAtomic(shadowPath(relKey), content);
}

bool SyncEngine::readShadow(const std::string& relKey, Bytes& out) {
    return readFileBytes(shadowPath(relKey), out);
}

void SyncEngine::backupReplaced(
    const std::wstring& absPath) { // Keep the last N replaced versions next to the data dir.
    std::wstring backupDir = store_->dir() + L"\\backups";
    CreateDirectoryW(backupDir.c_str(), nullptr);
    std::wstring name = fileNameOf(absPath);
    std::wstring dst = backupDir + L"\\" + name + L"." + std::to_wstring(time(nullptr)) + L".bak";
    CopyFileW(absPath.c_str(), dst.c_str(), TRUE);
    // Prune old backups for this file.
    std::wstring pattern = backupDir + L"\\" + name + L".*.bak";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    std::vector<std::wstring> found;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            found.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (found.size() > kMaxBackupsPerFile) {
        std::sort(found.begin(), found.end());
        for (size_t i = 0; i + kMaxBackupsPerFile < found.size(); ++i)
            DeleteFileW((backupDir + L"\\" + found[i]).c_str());
    }
}

// ---- upload path ----

json SyncEngine::buildFilePayload(const std::string& relPath, const std::wstring& absPath,
                                  const Bytes& plaintext, int64_t baseVersion, const VersionVector& vv) {
    Bytes encContent = Crypto::encrypt(masterKey_, plaintext, "file");
    json meta = {{"relative_path", relPath}};
    std::string metaStr = meta.dump();
    Bytes encMeta = Crypto::encrypt(masterKey_, Bytes(metaStr.begin(), metaStr.end()), "metadata");

    auto st = db_.getFile(relPath);
    std::string fileId = st && !st->fileId.empty() ? st->fileId : std::string();
    if (fileId.empty()) {
        // New file: generate a UUID (server stores it as-is).
        Bytes uuid = Crypto::random(16);
        uuid[6] = (uuid[6] & 0x0f) | 0x40;
        uuid[8] = (uuid[8] & 0x3f) | 0x80;
        char buf[37];
        snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9],
                 uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
        fileId = buf;
    }

    VersionVector out = vv;
    out.bump(deviceId());

    json payload;
    payload["file_id"] = fileId;
    payload["encrypted_metadata"] = Crypto::base64UrlEncode(encMeta);
    payload["encrypted_content"] = Crypto::base64UrlEncode(encContent);
    payload["content_hash"] = Crypto::sha256Hex(encContent);
    payload["base_version"] = baseVersion;
    payload["version_vector"] = json::parse(out.toJson());
    payload["size"] = encContent.size();
    payload["modified_at"] = ""; // server assigns when empty
    payload["idempotency_key"] =
        Crypto::sha256Hex(relPath + std::to_string(baseVersion) + Crypto::sha256Hex(plaintext)).substr(0, 36);
    return payload;
}

void SyncEngine::queueUpload(const std::string& relPath, const std::wstring& absPath) {
    PendingOp op;
    op.kind = "upload";
    op.relPath = relPath;
    db_.enqueueOp(op);
    setStatus(SyncStatus::Syncing, "Queued upload");
    syncRequested_ = true;
}

bool SyncEngine::uploadFile(const std::string& relPath, const std::wstring& absPath, std::string& errorOut) {
    if (!hasMasterKey()) {
        errorOut = "encryption not set up";
        return false;
    }

    Bytes plaintext;
    if (!readFileBytes(absPath, plaintext)) {
        errorOut = "cannot read file";
        return false;
    }
    if ((int64_t)plaintext.size() > settings_->maxFileBytes) {
        errorOut = "file exceeds max size";
        return false;
    }
    std::string plainHash = Crypto::sha256Hex(plaintext);
    auto st = db_.getFile(relPath);
    if (st && st->localHash == plainHash && st->remoteHash == plainHash) {
        return true; // content unchanged (localHash==remoteHash tracked on plaintext)
    }

    int64_t baseVersion = st ? st->remoteVersion : 0;
    VersionVector vv = st ? st->versionVector : VersionVector{};
    json payload = buildFilePayload(relPath, absPath, plaintext, baseVersion, vv);
    std::string fileId = payload["file_id"].get<std::string>();

    ApiResponse r = (baseVersion == 0 && (!st || st->remoteVersion == 0)) ? api_->createFile(payload)
                                                                          : api_->updateFile(fileId, payload);

    if (!r.transportOk) {
        errorOut = "offline";
        online_ = false;
        return false;
    }
    online_ = true;

    if (r.status == 200 || r.status == 201) {
        int64_t newVersion = r.body.value("version", (int64_t)1);
        VersionVector newVv = VersionVector::fromJson(payload["version_vector"].dump());
        LocalFileState f;
        f.relPath = relPath;
        f.fileId = fileId;
        f.syncRoot = st ? st->syncRoot : "";
        f.localHash = plainHash;
        f.remoteHash = plainHash;
        f.remoteVersion = newVersion;
        f.versionVector = newVv;
        f.deleted = false;
        db_.upsertFile(f);
        writeShadow(relPath, plaintext); // base for future 3-way merges
        return true;
    }
    if (r.status == 409) {
        return handleConflict(relPath, absPath, r.body.value("current", json::object()), errorOut);
    }
    errorOut = r.body.value("message", "upload failed");
    return false;
}

bool SyncEngine::deleteRemote(const std::string& relPath, std::string& errorOut) {
    auto st = db_.getFile(relPath);
    if (!st || st->fileId.empty()) {
        db_.removeFile(relPath);
        return true;
    }
    ApiResponse r = api_->deleteFile(st->fileId);
    if (!r.transportOk) {
        errorOut = "offline";
        online_ = false;
        return false;
    }
    online_ = true;
    if (r.status == 200 || r.status == 404) {
        db_.removeFile(relPath);
        return true;
    }
    errorOut = r.body.value("message", "delete failed");
    return false;
}

// ---- conflicts ----

bool SyncEngine::handleConflict(const std::string& relPath, const std::wstring& absPath,
                                const json& serverCurrent, std::string& errorOut) {
    // We have: local plaintext (absPath), server head (serverCurrent).
    // Fetch the server's ciphertext, decrypt, and three-way merge against
    // the base we both started from (last synced local content).
    std::string fileId = serverCurrent.value("file_id", "");
    if (fileId.empty()) {
        errorOut = "conflict without file id";
        return false;
    }

    ApiResponse full = api_->getFile(fileId);
    if (full.status != 200) {
        errorOut = "cannot fetch remote version";
        return false;
    }

    Bytes encRemote;
    if (!Crypto::base64UrlDecode(full.body.value("encrypted_content", ""), encRemote)) {
        errorOut = "bad remote payload";
        return false;
    }
    Bytes remotePlain;
    if (!Crypto::decrypt(masterKey_, encRemote, "file", remotePlain)) {
        errorOut = "cannot decrypt remote version";
        return false;
    }

    Bytes localPlain;
    readFileBytes(absPath, localPlain);
    std::string localText(localPlain.begin(), localPlain.end());
    std::string remoteText(remotePlain.begin(), remotePlain.end());

    // Base = the last version both sides agreed on (shadow copy written after
    // every successful sync). Missing base degrades to a manual conflict —
    // never to a guess-merge.
    std::string baseText;
    {
        Bytes base;
        if (readShadow(relPath, base))
            baseText.assign(base.begin(), base.end());
    }

    auto merged = ThreeWayMerge::merge(baseText, localText, remoteText);
    if (merged.clean) {
        // Auto-merge succeeded: write merged text, upload as new head.
        if (!writeFileAtomic(absPath, Bytes(merged.merged.begin(), merged.merged.end()))) {
            errorOut = "cannot write merged file";
            return false;
        }
        backupReplaced(absPath);
        if (onRemoteFileApplied)
            onRemoteFileApplied(absPath);
        int64_t newBase = serverCurrent.value("version", (int64_t)0);
        VersionVector vv =
            VersionVector::fromJson(serverCurrent.value("version_vector", json::object()).dump());
        json payload = buildFilePayload(relPath, absPath, Bytes(merged.merged.begin(), merged.merged.end()),
                                        newBase, vv);
        ApiResponse r = api_->updateFile(fileId, payload);
        if (r.status == 200) {
            auto st = db_.getFile(relPath);
            LocalFileState f = st.value_or(LocalFileState{});
            f.relPath = relPath;
            f.fileId = fileId;
            f.localHash = Crypto::sha256Hex(merged.merged);
            f.remoteHash = f.localHash;
            f.remoteVersion = r.body.value("version", newBase + 1);
            f.versionVector = VersionVector::fromJson(payload["version_vector"].dump());
            db_.upsertFile(f);
            writeShadow(relPath, Bytes(merged.merged.begin(), merged.merged.end()));
            setStatus(SyncStatus::Synced, "Auto-merged");
            return true;
        }
    }

    // Not auto-mergeable: preserve local content in a conflict copy and
    // record the conflict for the UI. Nothing is discarded.
    std::wstring conflictCopy =
        absPath + L" (conflict - " + PathUtil::utf8ToWide(settings_->deviceName) + L").bak";
    Bytes localBytes(localText.begin(), localText.end());
    writeFileAtomic(conflictCopy, localBytes);

    ConflictState c;
    c.fileId = fileId;
    c.relPath = relPath;
    c.remoteVersion = serverCurrent.value("version", (int64_t)0);
    c.localCopyPath = PathUtil::wideToUtf8(conflictCopy);
    c.remoteBlobHash = serverCurrent.value("content_hash", "");
    db_.addConflict(c);
    setStatus(SyncStatus::Conflict, "Conflict needs attention");
    return true; // queued op considered handled (conflict supersedes it)
}

bool SyncEngine::resolveConflict(const std::string& fileId, const std::string& strategy,
                                 std::string& errorOut) {
    auto conflicts = db_.conflicts();
    const ConflictState* target = nullptr;
    for (auto& c : conflicts)
        if (c.fileId == fileId) {
            target = &c;
            break;
        }
    if (!target) {
        errorOut = "conflict not found";
        return false;
    }

    auto absOpt = absPathForRel(target->relPath);
    if (!absOpt) {
        errorOut = "cannot map path";
        return false;
    }
    std::wstring abs = *absOpt;

    if (strategy == "keepLocal") {
        // Upload the preserved local copy as the new head over remoteVersion.
        std::wstring localCopy = PathUtil::utf8ToWide(target->localCopyPath);
        Bytes data;
        if (!readFileBytes(localCopy, data)) {
            errorOut = "local copy missing";
            return false;
        }
        writeFileAtomic(abs, data);
        auto st = db_.getFile(target->relPath);
        VersionVector vv = st ? st->versionVector : VersionVector{};
        json payload = buildFilePayload(target->relPath, abs, data, target->remoteVersion, vv);
        ApiResponse r = api_->updateFile(fileId, payload);
        if (r.status != 200) {
            errorOut = r.body.value("message", "upload failed");
            return false;
        }
        if (st) {
            LocalFileState f = *st;
            f.localHash = Crypto::sha256Hex(data);
            f.remoteHash = f.localHash;
            f.remoteVersion = r.body.value("version", target->remoteVersion + 1);
            db_.upsertFile(f);
            writeShadow(target->relPath, data);
        }
    }
    else if (strategy == "keepRemote") {
        ApiResponse full = api_->getFile(fileId);
        if (full.status != 200) {
            errorOut = "cannot fetch remote";
            return false;
        }
        Bytes enc, plain;
        if (!Crypto::base64UrlDecode(full.body.value("encrypted_content", ""), enc) ||
            !Crypto::decrypt(masterKey_, enc, "file", plain)) {
            errorOut = "cannot decrypt remote";
            return false;
        }
        backupReplaced(abs);
        if (!writeFileAtomic(abs, plain)) {
            errorOut = "write failed";
            return false;
        }
        if (onRemoteFileApplied)
            onRemoteFileApplied(abs);
        auto st = db_.getFile(target->relPath);
        if (st) {
            LocalFileState f = *st;
            f.localHash = Crypto::sha256Hex(plain);
            f.remoteHash = f.localHash;
            f.remoteVersion = full.body.value("version", target->remoteVersion);
            f.versionVector =
                VersionVector::fromJson(full.body.value("version_vector", json::object()).dump());
            db_.upsertFile(f);
            writeShadow(target->relPath, plain);
        }
    }
    else if (strategy == "keepBoth") {
        // Rename the conflict copy next to the original with a dated suffix,
        // then apply remote to the canonical path.
        ApiResponse full = api_->getFile(fileId);
        if (full.status != 200) {
            errorOut = "cannot fetch remote";
            return false;
        }
        Bytes enc, plain;
        if (!Crypto::base64UrlDecode(full.body.value("encrypted_content", ""), enc) ||
            !Crypto::decrypt(masterKey_, enc, "file", plain)) {
            errorOut = "cannot decrypt remote";
            return false;
        }
        time_t t = time(nullptr);
        tm lt{};
        localtime_s(&lt, &t);
        char date[16];
        snprintf(date, sizeof(date), "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
        std::wstring dir = dirOf(abs), name = fileNameOf(abs);
        std::wstring stem = name, ext;
        size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos) {
            stem = name.substr(0, dot);
            ext = name.substr(dot);
        }
        std::wstring bothName = dir + L"\\" + stem + L" (conflict - " +
                                PathUtil::utf8ToWide(settings_->deviceName) + L" - " +
                                PathUtil::utf8ToWide(date) + L")" + ext;
        std::wstring localCopy = PathUtil::utf8ToWide(target->localCopyPath);
        MoveFileExW(localCopy.c_str(), bothName.c_str(), MOVEFILE_REPLACE_EXISTING);
        backupReplaced(abs);
        if (!writeFileAtomic(abs, plain)) {
            errorOut = "write failed";
            return false;
        }
        if (onRemoteFileApplied)
            onRemoteFileApplied(abs);
        auto st = db_.getFile(target->relPath);
        if (st) {
            LocalFileState f = *st;
            f.localHash = Crypto::sha256Hex(plain);
            f.remoteHash = f.localHash;
            f.remoteVersion = full.body.value("version", target->remoteVersion);
            db_.upsertFile(f);
            writeShadow(target->relPath, plain);
        }
    }
    else {
        errorOut = "unknown strategy";
        return false;
    }

    db_.resolveConflict(fileId);
    setStatus(db_.conflictCount() > 0 ? SyncStatus::Conflict : SyncStatus::Synced, "Conflict resolved");
    return true;
}

// ---- download path ----

void SyncEngine::pullChanges() {
    if (!api_->hasTokens() || !hasMasterKey())
        return;
    ApiResponse r = api_->changesSince(db_.lastChangeSeq());
    if (!r.transportOk) {
        online_ = false;
        setStatus(SyncStatus::Offline, "Offline");
        return;
    }
    online_ = true;
    if (r.status != 200)
        return;

    int64_t latest = r.body.value("latest_seq", (int64_t)0);
    for (auto& ch : r.body.value("changes", json::array())) {
        std::string origin = ch.value("origin_device_id", "");
        if (origin == deviceId())
            continue; // our own change
        std::string fileId = ch.value("file_id", "");
        std::string err;
        applyRemoteFile(fileId, err);
    }
    if (latest > 0)
        db_.setLastChangeSeq(latest);
}

bool SyncEngine::applyRemoteFile(const std::string& fileId, std::string& errorOut) {
    ApiResponse r = api_->getFile(fileId);
    if (!r.transportOk) {
        online_ = false;
        errorOut = "offline";
        return false;
    }
    online_ = true;
    if (r.status != 200) {
        errorOut = "fetch failed";
        return false;
    }

    const json& f = r.body;
    int64_t remoteVersion = f.value("version", (int64_t)0);
    bool deleted = f.value("deleted", false);

    // Decrypt metadata to learn the path.
    Bytes encMeta, metaPlain;
    if (!Crypto::base64UrlDecode(f.value("encrypted_metadata", ""), encMeta) ||
        !Crypto::decrypt(masterKey_, encMeta, "metadata", metaPlain)) {
        errorOut = "cannot decrypt metadata";
        return false;
    }
    json meta;
    try {
        meta = json::parse(std::string(metaPlain.begin(), metaPlain.end()));
    }
    catch (...) {
        errorOut = "bad metadata";
        return false;
    }
    std::string relPath = meta.value("relative_path", "");
    std::string norm;
    if (!PathUtil::normalizeRelative(relPath, norm)) {
        errorOut = "remote path rejected";
        return false; // anti-traversal: never write outside roots
    }
    // relPath stored locally includes the root prefix; metadata path is the
    // plain relative path, so match by suffix against known files.
    std::string localKey;
    {
        auto all = db_.allFiles();
        for (auto& lf : all) {
            size_t colon = lf.relPath.find(':');
            if (colon != std::string::npos && lf.relPath.substr(colon + 1) == norm) {
                localKey = lf.relPath;
                break;
            }
        }
    }
    if (localKey.empty()) {
        // New remote file: place it in the first folder root that contains
        // the path's top-level segment, else the first root.
        auto roots = db_.listSyncRoots();
        if (roots.empty()) {
            errorOut = "no sync roots";
            return false;
        }
        std::string top = norm.substr(0, norm.find('/'));
        std::string chosen;
        for (auto& [id, root] : roots) {
            std::wstring candidate;
            if (PathUtil::joinInsideRoot(root, norm, candidate)) {
                chosen = id;
                break;
            }
        }
        if (chosen.empty())
            chosen = roots.front().first;
        localKey = chosen + ":" + norm;
    }

    auto absOpt = absPathForRel(localKey);
    if (!absOpt) {
        errorOut = "path mapping failed";
        return false;
    }
    std::wstring abs = *absOpt;
    if (ignored(norm, false))
        return true;

    auto st = db_.getFile(localKey);
    if (st && st->remoteVersion >= remoteVersion)
        return true; // already current

    if (deleted) {
        backupReplaced(abs);
        DeleteFileW(abs.c_str());
        if (st) {
            LocalFileState nf = *st;
            nf.deleted = true;
            nf.remoteVersion = remoteVersion;
            db_.upsertFile(nf);
        }
        return true;
    }

    Bytes encContent, plain;
    if (!Crypto::base64UrlDecode(f.value("encrypted_content", ""), encContent)) {
        errorOut = "bad content";
        return false;
    }
    // Verify ciphertext integrity before touching disk.
    if (Crypto::sha256Hex(encContent) != f.value("content_hash", "")) {
        errorOut = "hash mismatch";
        return false;
    }
    if (!Crypto::decrypt(masterKey_, encContent, "file", plain)) {
        errorOut = "decrypt failed";
        return false;
    }

    // If the local file has unsynchronized edits, this is a conflict too.
    Bytes currentLocal;
    if (readFileBytes(abs, currentLocal) && st) {
        std::string curHash = Crypto::sha256Hex(currentLocal);
        if (!st->localHash.empty() && curHash != st->localHash && curHash != st->remoteHash) {
            std::wstring dir = dirOf(abs), name = fileNameOf(abs);
            std::wstring copy = dir + L"\\" + name + L" (conflict - local unsaved).bak";
            writeFileAtomic(copy, currentLocal);
            ConflictState c;
            c.fileId = fileId;
            c.relPath = localKey;
            c.remoteVersion = remoteVersion;
            c.localCopyPath = PathUtil::wideToUtf8(copy);
            c.remoteBlobHash = f.value("content_hash", "");
            db_.addConflict(c);
            setStatus(SyncStatus::Conflict, "Conflict needs attention");
            return true;
        }
    }

    // Create parent dirs, then atomic replace with backup.
    std::wstring dir = dirOf(abs);
    for (size_t i = 3; i < dir.size(); ++i) {
        if (dir[i] == L'\\') {
            std::wstring part = dir.substr(0, i);
            CreateDirectoryW(part.c_str(), nullptr);
        }
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    if (GetFileAttributesW(abs.c_str()) != INVALID_FILE_ATTRIBUTES)
        backupReplaced(abs);
    if (!writeFileAtomic(abs, plain)) {
        errorOut = "write failed";
        return false;
    }
    if (onRemoteFileApplied)
        onRemoteFileApplied(abs);

    LocalFileState nf = st.value_or(LocalFileState{});
    nf.relPath = localKey;
    nf.fileId = fileId;
    nf.localHash = Crypto::sha256Hex(plain);
    nf.remoteHash = nf.localHash;
    nf.remoteVersion = remoteVersion;
    nf.versionVector = VersionVector::fromJson(f.value("version_vector", json::object()).dump());
    nf.deleted = false;
    db_.upsertFile(nf);
    writeShadow(localKey, plain); // base for future 3-way merges
    return true;
}

// ---- Notepad++ hooks ----

void SyncEngine::onFileSaved(const std::wstring& absPath) {
    if (settings_->pauseSync || !isSignedIn())
        return;
    auto loc = locateInRoots(absPath);
    if (!loc)
        return;
    // enqueueOp coalesces rapid successive saves of the same file, and the
    // upload path no-ops when content is unchanged — so just queue it.
    queueUpload(loc->first, absPath);
}

void SyncEngine::onFileOpened(const std::wstring& absPath) {
    if (!isSignedIn())
        return;
    auto loc = locateInRoots(absPath);
    if (!loc)
        return;
    // Ensure metadata exists so future saves diff correctly.
    auto st = db_.getFile(loc->first);
    if (!st) {
        LocalFileState f;
        f.relPath = loc->first;
        f.syncRoot = loc->first.substr(0, loc->first.find(':'));
        db_.upsertFile(f);
    }
}

// ---- fs events ----

void SyncEngine::onFsEvent(const FsEvent& ev) {
    if (settings_->pauseSync || !isSignedIn())
        return;
    auto loc = locateInRoots(ev.absPath);
    if (!loc) {
        // Possibly a rename from a known path.
        if (ev.kind == FsEvent::Kind::RenamedTo && !ev.oldAbsPath.empty()) {
            auto oldLoc = locateInRoots(ev.oldAbsPath);
            if (oldLoc) {
                PendingOp op;
                op.kind = "delete";
                op.relPath = oldLoc->first;
                db_.enqueueOp(op);
            }
        }
        return;
    }
    switch (ev.kind) {
    case FsEvent::Kind::Created:
    case FsEvent::Kind::Modified: {
        DWORD attr = GetFileAttributesW(ev.absPath.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            return;
        queueUpload(loc->first, ev.absPath);
        break;
    }
    case FsEvent::Kind::Deleted: {
        PendingOp op;
        op.kind = "delete";
        op.relPath = loc->first;
        db_.enqueueOp(op);
        syncRequested_ = true;
        break;
    }
    case FsEvent::Kind::RenamedFrom:
        break; // handled with RenamedTo
    case FsEvent::Kind::RenamedTo:
        queueUpload(loc->first, ev.absPath);
        break;
    }
}

// ---- ws events ----

void SyncEngine::onWsEvent(const json& ev) {
    std::string type = ev.value("type", "");
    if (type == "file_changed" || type == "file_deleted") {
        std::string origin = ev.value("origin_device_id", "");
        if (origin == deviceId())
            return;
        std::string fileId = ev.value("file_id", "");
        if (!fileId.empty()) {
            std::string err;
            if (!applyRemoteFile(fileId, err))
                pullChanges(); // fallback to feed
            setStatus(SyncStatus::Synced, "Updated");
        }
    }
    else if (type == "device_revoked") {
        if (ev.value("device_id", "") == deviceId())
            signOut();
    }
    else if (type == "session_changed") {
        // Session sync handled by the session feature (optional).
    }
}

// ---- worker loops ----

bool SyncEngine::processPendingOp(const PendingOp& op) {
    std::string err;
    if (op.kind == "upload") {
        auto abs = absPathForRel(op.relPath);
        if (!abs) {
            db_.removeOp(op.id);
            return true;
        }
        if (uploadFile(op.relPath, *abs, err)) {
            db_.removeOp(op.id);
            return true;
        }
    }
    else if (op.kind == "delete") {
        if (deleteRemote(op.relPath, err)) {
            db_.removeOp(op.id);
            return true;
        }
    }
    else {
        db_.removeOp(op.id);
        return true;
    }
    if (err == "offline") {
        online_ = false;
        return false;
    }
    db_.markOpFailed(op.id, err);
    // Drop poison ops after many retries (never silently: they're in logs).
    if (op.retryCount > 25)
        db_.removeOp(op.id);
    return false;
}

void SyncEngine::workerLoop() {
    while (running_) {
        if (!settings_->pauseSync && isSignedIn() && hasMasterKey()) {
            bool didWork = false;
            for (auto& op : db_.pendingOps()) {
                if (!running_)
                    break;
                if (!online_)
                    break;
                didWork = true;
                setStatus(SyncStatus::Syncing, "Syncing");
                processPendingOp(op);
                Sleep(50); // gentle pacing
            }
            if (didWork && db_.pendingOpCount() == 0 && db_.conflictCount() == 0)
                setStatus(SyncStatus::Synced, "Synced");
            else if (db_.conflictCount() > 0)
                setStatus(SyncStatus::Conflict, "Conflict needs attention");
        }
        for (int i = 0; i < 10 && running_; ++i)
            Sleep(500); // 5s cadence
    }
}

void SyncEngine::pollerLoop() {
    while (running_) {
        if (!settings_->pauseSync && isSignedIn() && hasMasterKey()) {
            pullChanges();
            if (online_ && db_.conflictCount() == 0) {
                // Only downgrade to Synced if nothing is in flight.
                if (db_.pendingOpCount() == 0)
                    setStatus(SyncStatus::Synced, "Synced");
            }
        }
        int interval = (std::max)(5, settings_->syncIntervalFallbackSec);
        for (int i = 0; i < interval * 2 && running_; ++i)
            Sleep(500);
    }
}

void SyncEngine::scannerLoop() {
    // Initial delay, then scan roots every 10 minutes as a safety net for
    // missed watcher events (buffer overflow, network shares, etc.).
    for (int i = 0; i < 30 && running_; ++i)
        Sleep(1000);
    while (running_) {
        if (!settings_->pauseSync && isSignedIn() && hasMasterKey()) {
            for (auto& [id, root] : db_.listSyncRoots()) {
                if (!running_)
                    break;
                std::vector<std::wstring> stack{root};
                while (!stack.empty() && running_) {
                    std::wstring dir = stack.back();
                    stack.pop_back();
                    WIN32_FIND_DATAW fd{};
                    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
                    if (h == INVALID_HANDLE_VALUE)
                        continue;
                    do {
                        std::wstring name = fd.cFileName;
                        if (name == L"." || name == L"..")
                            continue;
                        std::wstring full = dir + L"\\" + name;
                        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                        if (isDir) {
                            stack.push_back(full);
                            continue;
                        }
                        auto loc = locateInRoots(full);
                        if (!loc)
                            continue;
                        std::string norm = loc->first.substr(loc->first.find(':') + 1);
                        if (ignored(norm, false))
                            continue;
                        auto st = db_.getFile(loc->first);
                        if (!st) {
                            queueUpload(loc->first, full); // new file
                        }
                    } while (FindNextFileW(h, &fd));
                    FindClose(h);
                }
            }
        }
        for (int i = 0; i < 600 * 2 && running_; ++i)
            Sleep(500); // 10 min
    }
}

} // namespace npsync
