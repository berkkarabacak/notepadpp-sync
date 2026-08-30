// LocalDb.h — SQLite-backed local sync state.
//
// Stores everything the plugin needs to work offline and resume safely:
// account/device IDs, sync roots, per-file IDs and hashes, remote versions,
// the pending-operation queue (survives restarts), and conflict state.
// All data lives in %APPDATA%\Notepad++Sync\sync.db by default.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/VersionVector.h"

struct sqlite3;

namespace npsync {

struct LocalFileState {
    std::string fileId;          // empty until the server knows the file
    std::string syncRoot;        // root ID this file belongs to
    std::string relPath;         // normalized, forward slashes
    std::string localHash;       // sha256 of local *plaintext* content
    std::string remoteHash;      // sha256 of last synced ciphertext
    int64_t remoteVersion = 0;   // server version of last synced state
    VersionVector versionVector; // as of last synced state
    bool deleted = false;
    int64_t modifiedAtLocal = 0; // file mtime (informational only)
};

struct PendingOp {
    int64_t id = 0;
    std::string kind;    // "upload" | "delete" | "rename"
    std::string relPath;
    std::string newRelPath; // for rename
    int64_t queuedAt = 0;
    int retryCount = 0;
    std::string lastError;
};

struct ConflictState {
    std::string fileId;
    std::string relPath;
    int64_t remoteVersion = 0;
    std::string localCopyPath;  // absolute path of preserved local version
    std::string remoteBlobHash; // content hash of remote version to fetch
    int64_t detectedAt = 0;
};

class LocalDb {
public:
    LocalDb() = default;
    ~LocalDb();

    bool open(const std::wstring& path);
    void close();
    bool isOpen() const { return db_ != nullptr; }

    // ---- account / device ----
    bool setMeta(const std::string& key, const std::string& value);
    std::optional<std::string> getMeta(const std::string& key);

    // ---- sync roots ----
    bool addSyncRoot(const std::string& id, const std::wstring& absPath, bool isFolder);
    bool removeSyncRoot(const std::string& id);
    std::vector<std::pair<std::string, std::wstring>> listSyncRoots(); // (id, path) folders
    std::vector<std::pair<std::string, std::wstring>> listSyncFiles(); // (id, path) files

    // ---- file state ----
    bool upsertFile(const LocalFileState& f);
    std::optional<LocalFileState> getFile(const std::string& relPath);
    std::vector<LocalFileState> allFiles();
    bool removeFile(const std::string& relPath);
    bool updateRemoteState(const std::string& relPath, const std::string& remoteHash,
                           int64_t remoteVersion, const VersionVector& vv);
    bool updateLocalHash(const std::string& relPath, const std::string& localHash,
                         int64_t mtime);

    // ---- pending ops (offline queue) ----
    bool enqueueOp(const PendingOp& op);
    std::vector<PendingOp> pendingOps();
    bool removeOp(int64_t id);
    bool markOpFailed(int64_t id, const std::string& err);
    int pendingOpCount();

    // ---- conflicts ----
    bool addConflict(const ConflictState& c);
    std::vector<ConflictState> conflicts();
    bool resolveConflict(const std::string& fileId);
    int conflictCount();

    // ---- sync bookkeeping ----
    bool setLastChangeSeq(int64_t seq);
    int64_t lastChangeSeq();

private:
    sqlite3* db_ = nullptr;
    bool exec(const char* sql);
    bool migrate();
};

} // namespace npsync
