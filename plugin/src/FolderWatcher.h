// FolderWatcher.h — filesystem change notification for sync roots using
// ReadDirectoryChangesW (no polling). Reports create/modify/delete/rename
// events to the sync engine on a background thread.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace npsync
{

struct FsEvent
{
    enum class Kind
    {
        Created,
        Modified,
        Deleted,
        RenamedFrom,
        RenamedTo
    } kind;
    std::wstring absPath;
    std::wstring oldAbsPath; // for rename pairs
};

class FolderWatcher {
  public:
    using Callback = std::function<void(const FsEvent&)>;

    FolderWatcher() = default;
    ~FolderWatcher();

    // Watch a directory tree. Multiple roots can be added.
    bool addRoot(const std::wstring& absDir);
    void clearRoots();

    void start(Callback cb);
    void stop();
    bool running() const {
        return running_;
    }

  private:
    void pump(std::wstring dir, HANDLE dirHandle, Callback cb);

    std::vector<std::wstring> roots_;
    std::vector<std::thread> threads_;
    std::vector<HANDLE> dirHandles_;
    std::atomic<bool> running_{false};
};

} // namespace npsync
