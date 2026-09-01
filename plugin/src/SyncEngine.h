// SyncEngine.h — orchestrates all synchronization work on background threads.
//
// Responsibilities:
//   * scan sync roots (initial + periodic), hash files, detect changes
//   * react to FolderWatcher events (debounced)
//   * flush the offline pending-op queue when connectivity allows
//   * pull remote changes (WebSocket push + /sync/changes catch-up)
//   * three-way merge on conflicts; never discard content
//   * apply remote files atomically (temp-write, hash-verify, rename) and
//     keep a small local backup history of replaced versions
//   * maintain the status shown in the UI
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ApiClient.h"
#include "FolderWatcher.h"
#include "LocalDb.h"
#include "Settings.h"
#include "core/Crypto.h"
#include "core/IgnoreRules.h"

namespace npsync
{

enum class SyncStatus
{
    SignedOut,
    Synced,
    Syncing,
    Paused,
    Offline,
    Conflict,
    Error,
};

struct StatusInfo
{
    SyncStatus status = SyncStatus::SignedOut;
    std::string statusText = "Signed out";
    std::string lastSyncTime;
    int filesSynchronized = 0;
    int pendingUploads = 0;
    int pendingDownloads = 0;
    int conflicts = 0;
};

class SyncEngine {
  public:
    SyncEngine();
    ~SyncEngine();

    // Wiring.
    void init(SettingsStore* store, Settings* settings);
    void start();
    void stop();

    // Auth lifecycle (called from UI actions).
    bool signIn(const std::string& email, const std::string& password, std::string& errorOut,
                bool createAccount);
    void signOut();
    bool isSignedIn() const;

    // Key management.
    bool hasMasterKey() const;
    bool generateMasterKeyIfNeeded(); // first-run setup
    bool unlockWithRecoveryKey(const std::string& recoveryKey);
    std::string exportRecoveryKeyWrapped(); // for showing once to the user
    bool pairNewDevice(std::string& codeOut, std::string& errorOut);
    bool approvePairing(const std::string& code, std::string& errorOut);
    // Poll once for approval of a previously requested code; on approval the
    // wrapped master key is unwrapped with the code and installed locally.
    bool completePairing(const std::string& code, std::string& errorOut);

    // Manual actions.
    void syncNow();
    void setPaused(bool paused);

    // ---- UI helpers (device & sync-root management) ----

    struct DeviceInfo {
        std::string id, name, createdAt, lastSeenAt;
        bool revoked = false;
        bool current = false;
    };
    bool listDevices(std::vector<DeviceInfo>& out, std::string& errorOut);
    bool revokeDeviceById(const std::string& id, std::string& errorOut);
    bool renameDeviceById(const std::string& id, const std::string& name, std::string& errorOut);

    // Sync-root management; restarts watchers and rescans after changes.
    void addSyncRootPath(const std::wstring& absPath, bool isFolder);
    bool removeSyncRootPath(const std::string& rootId);
    void reloadRoots();
    void saveSettings();

    // Conflict resolution ("keepLocal" uploads local as new head;
    // "keepRemote" downloads remote; "keepBoth" writes a conflict copy).
    bool resolveConflict(const std::string& fileId, const std::string& strategy, std::string& errorOut);

    // Status & events.
    StatusInfo status();
    std::function<void()> onStatusChanged;                                // UI refresh (marshalled by caller)
    std::function<void(const std::wstring& absPath)> onRemoteFileApplied; // UI: reload buffer if open

    // Notepad++ notifications.
    void onFileSaved(const std::wstring& absPath);
    void onFileOpened(const std::wstring& absPath);

    Settings* settings() {
        return settings_;
    }
    LocalDb* db() {
        return &db_;
    }

  private:
    // Worker threads.
    void workerLoop();  // pending ops + uploads
    void pollerLoop();  // periodic /sync/changes catch-up
    void scannerLoop(); // periodic full scan of sync roots

    // Core operations.
    void queueUpload(const std::string& relPath, const std::wstring& absPath);
    bool processPendingOp(const PendingOp& op);
    bool uploadFile(const std::string& relPath, const std::wstring& absPath, std::string& errorOut);
    bool deleteRemote(const std::string& relPath, std::string& errorOut);
    void pullChanges();
    bool applyRemoteFile(const std::string& fileId, std::string& errorOut);
    bool handleConflict(const std::string& relPath, const std::wstring& absPath,
                        const nlohmann::json& serverCurrent, std::string& errorOut);

    // Helpers.
    std::optional<std::pair<std::string, std::wstring>> locateInRoots(const std::wstring& absPath);
    std::optional<std::wstring> absPathForRel(const std::string& relPath);
    bool ignored(const std::string& relPath, bool isDir);
    bool readFileBytes(const std::wstring& absPath, Bytes& out);
    bool writeFileAtomic(const std::wstring& absPath, const Bytes& data);
    void backupReplaced(const std::wstring& absPath);
    // Shadow copies of last-synced content provide the base for 3-way merges.
    std::wstring shadowPath(const std::string& relKey) const;
    void writeShadow(const std::string& relKey, const Bytes& content);
    bool readShadow(const std::string& relKey, Bytes& out);
    nlohmann::json buildFilePayload(const std::string& relPath, const std::wstring& absPath,
                                    const Bytes& plaintext, int64_t baseVersion, const VersionVector& vv);
    bool refreshIgnoreRules();
    void setStatus(SyncStatus s, const std::string& text);
    void onFsEvent(const FsEvent& ev);
    void onWsEvent(const nlohmann::json& ev);
    std::string deviceId() const;
    bool online();
    static std::string nowTimeString();

    SettingsStore* store_ = nullptr;
    Settings* settings_ = nullptr;
    LocalDb db_;
    std::unique_ptr<ApiClient> api_;
    std::unique_ptr<WsClient> ws_;
    FolderWatcher watcher_;

    Bytes masterKey_;
    mutable std::mutex mu_;
    std::atomic<bool> running_{false};
    std::atomic<bool> syncRequested_{false};
    std::atomic<bool> fsDirty_{false};
    std::atomic<bool> online_{true};
    std::thread workerThread_, pollerThread_, scannerThread_;

    std::map<std::string, IgnoreRules> rootRules_; // sync-root id -> rules
    std::mutex rulesMu_;

    StatusInfo status_;
    std::mutex statusMu_;
};

} // namespace npsync
