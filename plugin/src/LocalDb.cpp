#include "LocalDb.h"

#include "core/PathUtil.h"

#include <ctime>

#include <sqlite3.h>

namespace npsync {

namespace {

// Schema for the local state database. Versioned via user_version pragma.
const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS sync_roots (
    id       TEXT PRIMARY KEY,
    abs_path TEXT NOT NULL,      -- UTF-8 absolute path
    is_folder INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS files (
    rel_path        TEXT PRIMARY KEY,
    file_id         TEXT NOT NULL DEFAULT '',
    sync_root       TEXT NOT NULL DEFAULT '',
    local_hash      TEXT NOT NULL DEFAULT '',
    remote_hash     TEXT NOT NULL DEFAULT '',
    remote_version  INTEGER NOT NULL DEFAULT 0,
    version_vector  TEXT NOT NULL DEFAULT '{}',
    deleted         INTEGER NOT NULL DEFAULT 0,
    modified_local  INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS pending_ops (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    kind          TEXT NOT NULL,
    rel_path      TEXT NOT NULL,
    new_rel_path  TEXT NOT NULL DEFAULT '',
    queued_at     INTEGER NOT NULL,
    retry_count   INTEGER NOT NULL DEFAULT 0,
    last_error    TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS conflicts (
    file_id          TEXT PRIMARY KEY,
    rel_path         TEXT NOT NULL,
    remote_version   INTEGER NOT NULL,
    local_copy_path  TEXT NOT NULL DEFAULT '',
    remote_blob_hash TEXT NOT NULL DEFAULT '',
    detected_at      INTEGER NOT NULL
);
)SQL";

struct Stmt {
    sqlite3* db = nullptr;
    sqlite3_stmt* st = nullptr;
    Stmt(sqlite3* d, const char* sql) : db(d) {
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) st = nullptr;
    }
    ~Stmt() { if (st) sqlite3_finalize(st); }
    bool bind(int i, const std::string& v) {
        return sqlite3_bind_text(st, i, v.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
    }
    bool bind(int i, int64_t v) { return sqlite3_bind_int64(st, i, v) == SQLITE_OK; }
    bool bindW(int i, const std::wstring& v) { return bind(i, PathUtil::wideToUtf8(v)); }
    std::string colText(int i) const {
        const unsigned char* p = sqlite3_column_text(st, i);
        return p ? reinterpret_cast<const char*>(p) : "";
    }
    int64_t colInt(int i) const { return sqlite3_column_int64(st, i); }
    bool stepDone() { return sqlite3_step(st) == SQLITE_DONE; }
    bool stepRow() { return sqlite3_step(st) == SQLITE_ROW; }
};

int64_t nowUnix() {
    return static_cast<int64_t>(time(nullptr));
}

} // namespace

LocalDb::~LocalDb() { close(); }

bool LocalDb::open(const std::wstring& path) {
    close();
    std::string u8 = PathUtil::wideToUtf8(path);
    if (sqlite3_open_v2(u8.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        close();
        return false;
    }
    sqlite3_busy_timeout(db_, 5000);
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=NORMAL;");
    exec("PRAGMA foreign_keys=ON;");
    return migrate();
}

void LocalDb::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool LocalDb::exec(const char* sql) {
    return sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool LocalDb::migrate() {
    return exec(kSchema) && exec("PRAGMA user_version = 1;");
}

// ---- meta ----

bool LocalDb::setMeta(const std::string& key, const std::string& value) {
    Stmt st(db_, "INSERT INTO meta(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    return st.st && st.bind(1, key) && st.bind(2, value) && st.stepDone();
}

std::optional<std::string> LocalDb::getMeta(const std::string& key) {
    Stmt st(db_, "SELECT value FROM meta WHERE key=?");
    if (!st.st || !st.bind(1, key)) return std::nullopt;
    if (st.stepRow()) return st.colText(0);
    return std::nullopt;
}

// ---- sync roots ----

bool LocalDb::addSyncRoot(const std::string& id, const std::wstring& absPath, bool isFolder) {
    Stmt st(db_, "INSERT INTO sync_roots(id,abs_path,is_folder) VALUES(?,?,?) ON CONFLICT(id) DO UPDATE SET abs_path=excluded.abs_path");
    return st.st && st.bind(1, id) && st.bindW(2, absPath) && st.bind(3, isFolder ? 1 : 0) && st.stepDone();
}

bool LocalDb::removeSyncRoot(const std::string& id) {
    Stmt st(db_, "DELETE FROM sync_roots WHERE id=?");
    return st.st && st.bind(1, id) && st.stepDone();
}

std::vector<std::pair<std::string, std::wstring>> LocalDb::listSyncRoots() {
    Stmt st(db_, "SELECT id, abs_path FROM sync_roots WHERE is_folder=1");
    std::vector<std::pair<std::string, std::wstring>> out;
    if (!st.st) return out;
    while (st.stepRow()) out.emplace_back(st.colText(0), PathUtil::utf8ToWide(st.colText(1)));
    return out;
}

std::vector<std::pair<std::string, std::wstring>> LocalDb::listSyncFiles() {
    Stmt st(db_, "SELECT id, abs_path FROM sync_roots WHERE is_folder=0");
    std::vector<std::pair<std::string, std::wstring>> out;
    if (!st.st) return out;
    while (st.stepRow()) out.emplace_back(st.colText(0), PathUtil::utf8ToWide(st.colText(1)));
    return out;
}

// ---- files ----

bool LocalDb::upsertFile(const LocalFileState& f) {
    Stmt st(db_,
        "INSERT INTO files(rel_path,file_id,sync_root,local_hash,remote_hash,remote_version,version_vector,deleted,modified_local)"
        " VALUES(?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(rel_path) DO UPDATE SET file_id=excluded.file_id, sync_root=excluded.sync_root,"
        " local_hash=excluded.local_hash, remote_hash=excluded.remote_hash, remote_version=excluded.remote_version,"
        " version_vector=excluded.version_vector, deleted=excluded.deleted, modified_local=excluded.modified_local");
    return st.st && st.bind(1, f.relPath) && st.bind(2, f.fileId) && st.bind(3, f.syncRoot) &&
           st.bind(4, f.localHash) && st.bind(5, f.remoteHash) && st.bind(6, f.remoteVersion) &&
           st.bind(7, f.versionVector.toJson()) && st.bind(8, f.deleted ? 1 : 0) &&
           st.bind(9, f.modifiedAtLocal) && st.stepDone();
}

std::optional<LocalFileState> LocalDb::getFile(const std::string& relPath) {
    Stmt st(db_, "SELECT file_id,sync_root,local_hash,remote_hash,remote_version,version_vector,deleted,modified_local FROM files WHERE rel_path=?");
    if (!st.st || !st.bind(1, relPath)) return std::nullopt;
    if (!st.stepRow()) return std::nullopt;
    LocalFileState f;
    f.relPath = relPath;
    f.fileId = st.colText(0);
    f.syncRoot = st.colText(1);
    f.localHash = st.colText(2);
    f.remoteHash = st.colText(3);
    f.remoteVersion = st.colInt(4);
    f.versionVector = VersionVector::fromJson(st.colText(5));
    f.deleted = st.colInt(6) != 0;
    f.modifiedAtLocal = st.colInt(7);
    return f;
}

std::vector<LocalFileState> LocalDb::allFiles() {
    Stmt st(db_, "SELECT rel_path,file_id,sync_root,local_hash,remote_hash,remote_version,version_vector,deleted,modified_local FROM files");
    std::vector<LocalFileState> out;
    if (!st.st) return out;
    while (st.stepRow()) {
        LocalFileState f;
        f.relPath = st.colText(0);
        f.fileId = st.colText(1);
        f.syncRoot = st.colText(2);
        f.localHash = st.colText(3);
        f.remoteHash = st.colText(4);
        f.remoteVersion = st.colInt(5);
        f.versionVector = VersionVector::fromJson(st.colText(6));
        f.deleted = st.colInt(7) != 0;
        f.modifiedAtLocal = st.colInt(8);
        out.push_back(std::move(f));
    }
    return out;
}

bool LocalDb::removeFile(const std::string& relPath) {
    Stmt st(db_, "DELETE FROM files WHERE rel_path=?");
    return st.st && st.bind(1, relPath) && st.stepDone();
}

bool LocalDb::updateRemoteState(const std::string& relPath, const std::string& remoteHash,
                                int64_t remoteVersion, const VersionVector& vv) {
    Stmt st(db_, "UPDATE files SET remote_hash=?, remote_version=?, version_vector=? WHERE rel_path=?");
    return st.st && st.bind(1, remoteHash) && st.bind(2, remoteVersion) &&
           st.bind(3, vv.toJson()) && st.bind(4, relPath) && st.stepDone();
}

bool LocalDb::updateLocalHash(const std::string& relPath, const std::string& localHash, int64_t mtime) {
    Stmt st(db_, "UPDATE files SET local_hash=?, modified_local=? WHERE rel_path=?");
    return st.st && st.bind(1, localHash) && st.bind(2, mtime) && st.bind(3, relPath) && st.stepDone();
}

// ---- pending ops ----

bool LocalDb::enqueueOp(const PendingOp& op) {
    // Coalesce: an existing queued op for the same path is replaced.
    {
        Stmt del(db_, "DELETE FROM pending_ops WHERE rel_path=? AND kind=?");
        if (del.st) { del.bind(1, op.relPath); del.bind(2, op.kind); sqlite3_step(del.st); }
    }
    Stmt st(db_, "INSERT INTO pending_ops(kind,rel_path,new_rel_path,queued_at,retry_count,last_error) VALUES(?,?,?,?,0,'')");
    return st.st && st.bind(1, op.kind) && st.bind(2, op.relPath) &&
           st.bind(3, op.newRelPath) && st.bind(4, nowUnix()) && st.stepDone();
}

std::vector<PendingOp> LocalDb::pendingOps() {
    Stmt st(db_, "SELECT id,kind,rel_path,new_rel_path,queued_at,retry_count,last_error FROM pending_ops ORDER BY id");
    std::vector<PendingOp> out;
    if (!st.st) return out;
    while (st.stepRow()) {
        PendingOp op;
        op.id = st.colInt(0);
        op.kind = st.colText(1);
        op.relPath = st.colText(2);
        op.newRelPath = st.colText(3);
        op.queuedAt = st.colInt(4);
        op.retryCount = static_cast<int>(st.colInt(5));
        op.lastError = st.colText(6);
        out.push_back(std::move(op));
    }
    return out;
}

bool LocalDb::removeOp(int64_t id) {
    Stmt st(db_, "DELETE FROM pending_ops WHERE id=?");
    return st.st && st.bind(1, id) && st.stepDone();
}

bool LocalDb::markOpFailed(int64_t id, const std::string& err) {
    Stmt st(db_, "UPDATE pending_ops SET retry_count=retry_count+1, last_error=? WHERE id=?");
    return st.st && st.bind(1, err) && st.bind(2, id) && st.stepDone();
}

int LocalDb::pendingOpCount() {
    Stmt st(db_, "SELECT count(*) FROM pending_ops");
    if (!st.st || !st.stepRow()) return 0;
    return static_cast<int>(st.colInt(0));
}

// ---- conflicts ----

bool LocalDb::addConflict(const ConflictState& c) {
    Stmt st(db_, "INSERT INTO conflicts(file_id,rel_path,remote_version,local_copy_path,remote_blob_hash,detected_at)"
                 " VALUES(?,?,?,?,?,?) ON CONFLICT(file_id) DO UPDATE SET remote_version=excluded.remote_version,"
                 " local_copy_path=excluded.local_copy_path, remote_blob_hash=excluded.remote_blob_hash,"
                 " detected_at=excluded.detected_at");
    return st.st && st.bind(1, c.fileId) && st.bind(2, c.relPath) && st.bind(3, c.remoteVersion) &&
           st.bind(4, c.localCopyPath) && st.bind(5, c.remoteBlobHash) && st.bind(6, nowUnix()) && st.stepDone();
}

std::vector<ConflictState> LocalDb::conflicts() {
    Stmt st(db_, "SELECT file_id,rel_path,remote_version,local_copy_path,remote_blob_hash,detected_at FROM conflicts ORDER BY detected_at DESC");
    std::vector<ConflictState> out;
    if (!st.st) return out;
    while (st.stepRow()) {
        ConflictState c;
        c.fileId = st.colText(0);
        c.relPath = st.colText(1);
        c.remoteVersion = st.colInt(2);
        c.localCopyPath = st.colText(3);
        c.remoteBlobHash = st.colText(4);
        c.detectedAt = st.colInt(5);
        out.push_back(std::move(c));
    }
    return out;
}

bool LocalDb::resolveConflict(const std::string& fileId) {
    Stmt st(db_, "DELETE FROM conflicts WHERE file_id=?");
    return st.st && st.bind(1, fileId) && st.stepDone();
}

int LocalDb::conflictCount() {
    Stmt st(db_, "SELECT count(*) FROM conflicts");
    if (!st.st || !st.stepRow()) return 0;
    return static_cast<int>(st.colInt(0));
}

// ---- change seq ----

bool LocalDb::setLastChangeSeq(int64_t seq) { return setMeta("last_change_seq", std::to_string(seq)); }

int64_t LocalDb::lastChangeSeq() {
    auto v = getMeta("last_change_seq");
    if (!v) return 0;
    try { return std::stoll(*v); } catch (...) { return 0; }
}

} // namespace npsync
