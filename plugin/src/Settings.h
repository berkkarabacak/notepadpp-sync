// Settings.h — plugin settings, persisted under
// %APPDATA%\Notepad++Sync\settings.json. Secrets (tokens, wrapped master key)
// are stored via Windows DPAPI in a separate protected file, never in the
// plain settings file.
#pragma once

#include <string>
#include <vector>

namespace npsync
{

enum class SessionSyncMode
{
    FilesOnly = 0,
    FilesAndTabs = 1,
    FilesTabsCursor = 2,
};

struct Settings
{
    // General
    bool startSyncAutomatically = true;
    bool pauseSync = false;
    int syncIntervalFallbackSec = 30;
    bool webSocketEnabled = true;
    bool notificationsEnabled = true;

    // Files
    std::vector<std::wstring> syncRoots;          // absolute directories
    std::vector<std::wstring> syncFiles;          // individual files
    std::vector<std::string> extraIgnorePatterns; // in addition to .npsyncignore
    int64_t maxFileBytes = 100ll * 1024 * 1024;

    // Session
    SessionSyncMode sessionMode = SessionSyncMode::FilesOnly;
    bool syncUnsavedNotes = false; // dangerous opt-in, clearly warned in UI

    // Security
    std::string deviceName;
    std::string deviceId;
    std::string accountId;

    // Advanced
    std::string backendUrl = "https://sync.example.com";
    bool debugLogging = false;
    std::wstring databaseLocation; // empty = default
    int versionRetention = 30;
};

class SettingsStore {
  public:
    explicit SettingsStore(std::wstring appDataDir);

    bool load(Settings& out);
    bool save(const Settings& s);

    // DPAPI-protected secret storage (tokens + wrapped master key).
    bool saveSecret(const std::wstring& name, const std::string& value);
    bool loadSecret(const std::wstring& name, std::string& valueOut);
    bool deleteSecret(const std::wstring& name);

    const std::wstring& dir() const {
        return dir_;
    }

  private:
    std::wstring dir_;
    std::wstring settingsPath() const;
    std::wstring secretPath(const std::wstring& name) const;
};

} // namespace npsync
