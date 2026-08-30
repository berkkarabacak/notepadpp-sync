#include "Settings.h"
#include "core/PathUtil.h"

#include <fstream>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <wincrypt.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#pragma comment(lib, "crypt32.lib")

using nlohmann::json;

namespace npsync
{

SettingsStore::SettingsStore(std::wstring appDataDir) : dir_(std::move(appDataDir)) {
    CreateDirectoryW(dir_.c_str(), nullptr);
}

std::wstring SettingsStore::settingsPath() const {
    return dir_ + L"\\settings.json";
}

std::wstring SettingsStore::secretPath(const std::wstring& name) const {
    return dir_ + L"\\secrets\\" + name + L".bin";
}

bool SettingsStore::load(Settings& s) {
    std::ifstream f(settingsPath(), std::ios::binary);
    if (!f)
        return false;
    json j;
    try {
        f >> j;
    }
    catch (...) {
        return false;
    }
    s.startSyncAutomatically = j.value("start_sync_automatically", true);
    s.pauseSync = j.value("pause_sync", false);
    s.syncIntervalFallbackSec = j.value("sync_interval_fallback_sec", 30);
    s.webSocketEnabled = j.value("websocket_enabled", true);
    s.notificationsEnabled = j.value("notifications_enabled", true);
    s.maxFileBytes = j.value("max_file_bytes", 100ll * 1024 * 1024);
    s.sessionMode = static_cast<SessionSyncMode>(j.value("session_mode", 0));
    s.syncUnsavedNotes = j.value("sync_unsaved_notes", false);
    s.deviceName = j.value("device_name", std::string());
    s.deviceId = j.value("device_id", std::string());
    s.accountId = j.value("account_id", std::string());
    s.backendUrl = j.value("backend_url", "https://sync.example.com");
    s.debugLogging = j.value("debug_logging", false);
    s.versionRetention = j.value("version_retention", 30);
    std::string dbLoc = j.value("database_location", std::string());
    s.databaseLocation = dbLoc.empty() ? L"" : PathUtil::utf8ToWide(dbLoc);

    s.syncRoots.clear();
    for (auto& v : j.value("sync_roots", json::array()))
        s.syncRoots.push_back(PathUtil::utf8ToWide(v.get<std::string>()));
    s.syncFiles.clear();
    for (auto& v : j.value("sync_files", json::array()))
        s.syncFiles.push_back(PathUtil::utf8ToWide(v.get<std::string>()));
    s.extraIgnorePatterns = j.value("ignore_patterns", std::vector<std::string>{});
    return true;
}

bool SettingsStore::save(const Settings& s) {
    json j;
    j["start_sync_automatically"] = s.startSyncAutomatically;
    j["pause_sync"] = s.pauseSync;
    j["sync_interval_fallback_sec"] = s.syncIntervalFallbackSec;
    j["websocket_enabled"] = s.webSocketEnabled;
    j["notifications_enabled"] = s.notificationsEnabled;
    j["max_file_bytes"] = s.maxFileBytes;
    j["session_mode"] = static_cast<int>(s.sessionMode);
    j["sync_unsaved_notes"] = s.syncUnsavedNotes;
    j["device_name"] = s.deviceName;
    j["device_id"] = s.deviceId;
    j["account_id"] = s.accountId;
    j["backend_url"] = s.backendUrl;
    j["debug_logging"] = s.debugLogging;
    j["version_retention"] = s.versionRetention;
    j["database_location"] = PathUtil::wideToUtf8(s.databaseLocation);

    json roots = json::array();
    for (auto& r : s.syncRoots)
        roots.push_back(PathUtil::wideToUtf8(r));
    j["sync_roots"] = roots;
    json files = json::array();
    for (auto& r : s.syncFiles)
        files.push_back(PathUtil::wideToUtf8(r));
    j["sync_files"] = files;
    j["ignore_patterns"] = s.extraIgnorePatterns;

    // Atomic write: temp file + rename.
    std::wstring tmp = settingsPath() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f << j.dump(2);
    }
    return MoveFileExW(tmp.c_str(), settingsPath().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool SettingsStore::saveSecret(const std::wstring& name, const std::string& value) {
    CreateDirectoryW((dir_ + L"\\secrets").c_str(), nullptr);
    DATA_BLOB in{static_cast<DWORD>(value.size()), reinterpret_cast<BYTE*>(const_cast<char*>(value.data()))};
    DATA_BLOB out{};
    // DPAPI current-user protection: only this Windows user can decrypt.
    if (!CryptProtectData(&in, L"NPSync", nullptr, nullptr, nullptr, 0, &out))
        return false;
    std::wstring path = secretPath(name);
    std::wstring tmp = path + L".tmp";
    bool ok = false;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (f) {
            f.write(reinterpret_cast<const char*>(out.pbData), out.cbData);
            ok = f.good();
        }
    }
    LocalFree(out.pbData);
    if (!ok)
        return false;
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool SettingsStore::loadSecret(const std::wstring& name, std::string& valueOut) {
    std::ifstream f(secretPath(name), std::ios::binary);
    if (!f)
        return false;
    std::string enc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (enc.empty())
        return false;
    DATA_BLOB in{static_cast<DWORD>(enc.size()), reinterpret_cast<BYTE*>(enc.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
        return false;
    valueOut.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool SettingsStore::deleteSecret(const std::wstring& name) {
    return DeleteFileW(secretPath(name).c_str()) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND;
}

} // namespace npsync
