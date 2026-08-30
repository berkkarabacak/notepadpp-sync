#include "FolderWatcher.h"

#include <algorithm>

namespace npsync {

FolderWatcher::~FolderWatcher() { stop(); }

bool FolderWatcher::addRoot(const std::wstring& absDir) {
    roots_.push_back(absDir);
    return true;
}

void FolderWatcher::clearRoots() { roots_.clear(); }

void FolderWatcher::start(Callback cb) {
    stop();
    running_ = true;
    for (const auto& root : roots_) {
        HANDLE h = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        dirHandles_.push_back(h);
        threads_.emplace_back([this, root, h, cb] { pump(root, h, cb); });
    }
}

void FolderWatcher::stop() {
    running_ = false;
    // CancelIoEx wakes any thread blocked in ReadDirectoryChangesW.
    for (HANDLE h : dirHandles_) {
        CancelIoEx(h, nullptr);
        CloseHandle(h);
    }
    dirHandles_.clear();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
}

void FolderWatcher::pump(std::wstring dir, HANDLE dirHandle, Callback cb) {
    std::vector<uint8_t> buf(64 * 1024);
    std::wstring pendingOldName;

    while (running_) {
        DWORD bytes = 0;
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL ok = ReadDirectoryChangesW(dirHandle, buf.data(), (DWORD)buf.size(), TRUE,
                                        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                                        FILE_NOTIFY_CHANGE_CREATION,
                                        &bytes, &ov, nullptr);
        if (!ok) {
            CloseHandle(ov.hEvent);
            break;
        }
        // Wait with timeout so stop() is responsive even without CancelIoEx.
        DWORD wait = WaitForSingleObject(ov.hEvent, 500);
        if (wait == WAIT_TIMEOUT) {
            CancelIoEx(dirHandle, &ov);
            WaitForSingleObject(ov.hEvent, 100);
            CloseHandle(ov.hEvent);
            continue;
        }
        DWORD transferred = 0;
        if (!GetOverlappedResult(dirHandle, &ov, &transferred, FALSE)) {
            CloseHandle(ov.hEvent);
            if (!running_) break;
            continue;
        }
        CloseHandle(ov.hEvent);
        if (transferred == 0) continue; // buffer overflow -> engine does a full scan

        size_t offset = 0;
        for (;;) {
            auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf.data() + offset);
            std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            std::wstring full = dir + L"\\" + name;

            FsEvent ev;
            bool emit = true;
            switch (info->Action) {
            case FILE_ACTION_ADDED:            ev.kind = FsEvent::Kind::Created; break;
            case FILE_ACTION_REMOVED:          ev.kind = FsEvent::Kind::Deleted; break;
            case FILE_ACTION_MODIFIED:         ev.kind = FsEvent::Kind::Modified; break;
            case FILE_ACTION_RENAMED_OLD_NAME: ev.kind = FsEvent::Kind::RenamedFrom; break;
            case FILE_ACTION_RENAMED_NEW_NAME: ev.kind = FsEvent::Kind::RenamedTo; break;
            default: emit = false; break;
            }
            if (emit) {
                ev.absPath = full;
                // Pair rename old/new names (they arrive adjacently).
                if (ev.kind == FsEvent::Kind::RenamedFrom) {
                    pendingOldName = full;
                } else if (ev.kind == FsEvent::Kind::RenamedTo && !pendingOldName.empty()) {
                    ev.oldAbsPath = pendingOldName;
                    pendingOldName.clear();
                }
                cb(ev);
            }
            if (info->NextEntryOffset == 0) break;
            offset += info->NextEntryOffset;
        }
    }
}

} // namespace npsync
