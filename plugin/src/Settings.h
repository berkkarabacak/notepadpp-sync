// Settings.h — plugin settings, persisted under
// %APPDATA%\Notepad++Sync\settings.json. Secrets (tokens, wrapped master key)
// are stored via Windows DPAPI in a separate protected file, never in the
// plain settings file.
#pragma once

#include <string>
#include <vector>

namespace npsync
{

// Shipping Backend URL. A fresh install uses this host. Another server,
// including http://localhost:8080, is a settings override.
inline constexpr char kDefaultBackendUrl[] = "https://sync.berkkarabacak.com";

// Missing, empty, or the retired placeholder all mean "use the shipping default".
// Any other stored value, including a self-hosted URL, is kept.
inline std::string resolveBackendUrl(const std::string& stored, bool keyPresent) {
    if (!keyPresent || stored.empty() || stored == "https://sync.example.com")
        return kDefaultBackendUrl;
    return stored;
}

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

    // Advanced. Fresh installs use kDefaultBackendUrl. Any other saved URL,
    // including localhost, is kept. Save writes the file; a running Notepad++
    // keeps the URL it loaded at startup until it is restarted.
    std::string backendUrl = kDefaultBackendUrl;
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
