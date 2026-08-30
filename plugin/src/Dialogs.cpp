// Dialogs.cpp — native Win32 dialog implementations. Plain and small by
// design: standard controls, no custom chrome, system fonts.
#include "Dialogs.h"

#include "Logger.h"
#include "SyncEngine.h"
#include "core/PathUtil.h"

#include <commctrl.h>
#include <cwchar>
#include <objbase.h>
#include <shlobj.h>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace npsync {

namespace {

std::wstring widen(const std::string& s) { return PathUtil::utf8ToWide(s); }
std::string narrow(const std::wstring& s) { return PathUtil::wideToUtf8(s); }

std::wstring editText(HWND hEdit) {
    int len = GetWindowTextLengthW(hEdit);
    std::wstring out(len + 1, L'\0');
    GetWindowTextW(hEdit, out.data(), len + 1);
    out.resize(len);
    return out;
}

HWND makeLabel(HWND dlg, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                           x, y, w, h, dlg, nullptr, nullptr, nullptr);
}

HWND makeEdit(HWND dlg, int id, int x, int y, int w, int h, DWORD extraStyle = 0) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | extraStyle,
                           x, y, w, h, dlg, (HMENU)(intptr_t)id, nullptr, nullptr);
}

HWND makeButton(HWND dlg, int id, const wchar_t* text, int x, int y, int w, int h, DWORD style = BS_PUSHBUTTON) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, dlg, (HMENU)(intptr_t)id, nullptr, nullptr);
}

void setDefaultFont(HWND dlg) {
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    EnumChildWindows(dlg, [](HWND child, LPARAM lp) -> BOOL {
        SendMessageW(child, WM_SETFONT, (WPARAM)lp, TRUE);
        return TRUE;
    }, (LPARAM)font);
}

// ---- generic modal dialog host ----

struct DialogBase {
    SyncEngine* engine = nullptr;
    bool done = false;
};

DialogBase& ctx(HWND hwnd) {
    return *reinterpret_cast<DialogBase*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

} // namespace

// ---- Sign In ----

namespace {
enum {
    ID_SIGNIN_EMAIL = 100, ID_SIGNIN_PASSWORD, ID_SIGNIN_CREATE,
    ID_SIGNIN_OK, ID_SIGNIN_CANCEL, ID_SIGNIN_STATUS
};

INT_PTR CALLBACK signInProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        makeLabel(dlg, L"Email:", 12, 14, 80, 20);
        makeEdit(dlg, ID_SIGNIN_EMAIL, 100, 12, 240, 22);
        makeLabel(dlg, L"Password:", 12, 44, 80, 20);
        makeEdit(dlg, ID_SIGNIN_PASSWORD, 100, 42, 240, 22, ES_PASSWORD);
        makeButton(dlg, ID_SIGNIN_CREATE,
            L"Create a new account (instead of signing in)", 100, 74, 260, 20, BS_AUTOCHECKBOX);
        makeButton(dlg, ID_SIGNIN_OK, L"Sign In", 100, 104, 100, 26, BS_DEFPUSHBUTTON);
        makeButton(dlg, ID_SIGNIN_CANCEL, L"Cancel", 210, 104, 100, 26);
        HWND hStatus = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                       12, 140, 340, 20, dlg, (HMENU)(intptr_t)ID_SIGNIN_STATUS,
                                       nullptr, nullptr);
        (void)hStatus;
        setDefaultFont(dlg);
        return TRUE;
    }
    case WM_COMMAND: {
        switch (LOWORD(wp)) {
        case ID_SIGNIN_OK: {
            std::string email = narrow(editText(GetDlgItem(dlg, ID_SIGNIN_EMAIL)));
            std::string password = narrow(editText(GetDlgItem(dlg, ID_SIGNIN_PASSWORD)));
            bool create = SendMessageW(GetDlgItem(dlg, ID_SIGNIN_CREATE), BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (email.empty() || password.empty()) {
                SetDlgItemTextW(dlg, ID_SIGNIN_STATUS, L"Email and password are required.");
                return TRUE;
            }
            std::string err;
            if (ctx(dlg).engine->signIn(email, password, err, create)) {
                ctx(dlg).done = true;
                EndDialog(dlg, IDOK);
            } else {
                SetDlgItemTextW(dlg, ID_SIGNIN_STATUS, widen(err).c_str());
            }
            return TRUE;
        }
        case ID_SIGNIN_CANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

// A tiny dialog-template-in-memory builder (avoids .rc compilation for the
// simple dialogs; the resource script covers the version info).
struct DialogMemory {
    std::vector<uint8_t> buf;
    void begin(const wchar_t* title, int w, int h) {
        buf.resize(1024 * 4, 0);
        auto* d = reinterpret_cast<DLGTEMPLATE*>(buf.data());
        d->style = DS_SETFONT | DS_FIXEDSYS | WS_POPUP | WS_CAPTION | WS_SYSMENU;
        d->dwExtendedStyle = 0;
        d->cdit = 0;
        d->x = 10; d->y = 10;
        d->cx = (short)(w / 2); d->cy = (short)(h / 2);
        auto* p = reinterpret_cast<wchar_t*>(d + 1);
        *p++ = 0;                    // menu
        *p++ = 0;                    // class
        wcscpy_s(p, 64, title);      // title
        p += wcslen(p) + 1;
        *p++ = 8;                    // font size
        wcscpy_s(p, 16, L"MS Shell Dlg");
    }
    DLGTEMPLATE* get() { return reinterpret_cast<DLGTEMPLATE*>(buf.data()); }
};
} // namespace

bool Dialogs::showSignIn(HWND parent, SyncEngine& engine) {
    DialogMemory mem;
    mem.begin(L"Notepad++ Sync — Sign In", 370, 175);
    DialogBase base{&engine, false};
    INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(L"NppSync.dll"),
        mem.get(), parent, signInProc, reinterpret_cast<LPARAM>(&base));
    return r == IDOK && base.done;
}

// ---- Status ----

void Dialogs::showStatus(HWND parent, SyncEngine& engine) {
    StatusInfo st = engine.status();
    wchar_t buf[768];
    swprintf(buf, 768,
        L"Notepad++ Sync\n\nStatus: %hs\nLast sync: %hs\n\nFiles synchronized: %d\n"
        L"Pending uploads: %d\nPending downloads: %d\nConflicts: %d",
        st.statusText.c_str(),
        st.lastSyncTime.empty() ? "never" : st.lastSyncTime.c_str(),
        st.filesSynchronized, st.pendingUploads, st.pendingDownloads, st.conflicts);
    MessageBoxW(parent, buf, L"Notepad++ Sync — Status", MB_OK | MB_ICONINFORMATION);
}

// ---- Synced files/folders ----

void Dialogs::showSyncedFiles(HWND parent, SyncEngine& engine) {
    // Folder picker via the shell; added roots are watched immediately.
    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t path[MAX_PATH] = {0};
    BROWSEINFOW bi{};
    bi.hwndOwner = parent;
    bi.lpszTitle = L"Choose a folder to synchronize";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        if (SHGetPathFromIDListW(pidl, path)) {
            std::wstring id = std::to_wstring((unsigned long long)GetTickCount64());
            engine.db()->addSyncRoot(narrow(id), path, true);
            engine.syncNow();
            Logger::info("added sync root");
        }
        CoTaskMemFree(pidl);
    }
}

// ---- Conflicts ----

void Dialogs::showConflicts(HWND parent, SyncEngine& engine) {
    auto conflicts = engine.db()->conflicts();
    if (conflicts.empty()) {
        MessageBoxW(parent, L"No conflicts. Everything is in sync.",
                    L"Notepad++ Sync — Conflicts", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const ConflictState& c = conflicts.front();
    std::wstring text = L"Conflict in: " + widen(c.relPath) +
        L"\n\nBoth versions are preserved. Choose how to resolve:\n\n"
        L"  YES    — Keep Local (your copy wins, uploaded as new version)\n"
        L"  NO     — Keep Remote (download the other device's version)\n"
        L"  CANCEL — Keep Both (remote applied, your copy kept alongside)";
    int r = MessageBoxW(parent, text.c_str(), L"Notepad++ Sync — Resolve Conflict",
                        MB_YESNOCANCEL | MB_ICONWARNING);
    std::string strategy;
    if (r == IDYES) strategy = "keepLocal";
    else if (r == IDNO) strategy = "keepRemote";
    else if (r == IDCANCEL) strategy = "keepBoth";
    else return;
    std::string err;
    if (!engine.resolveConflict(c.fileId, strategy, err))
        MessageBoxW(parent, widen(err).c_str(), L"Notepad++ Sync", MB_OK | MB_ICONERROR);
}

// ---- Devices ----

void Dialogs::showDevices(HWND parent, SyncEngine& engine) {
    MessageBoxW(parent,
        L"Device management shows every device on your account and lets you "
        L"rename, pair, or revoke devices.\n\n"
        L"(Full list UI: see Settings → Security. Pairing: an existing device "
        L"approves the code shown on the new device.)",
        L"Notepad++ Sync — Devices", MB_OK | MB_ICONINFORMATION);
}

// ---- Settings ----

void Dialogs::showSettings(HWND parent, SyncEngine& engine) {
    Settings* s = engine.settings();
    std::wstring msg =
        L"Current settings:\n\n"
        L"Backend URL: " + widen(s->backendUrl) + L"\n"
        L"Device name: " + widen(s->deviceName) + L"\n"
        L"Sync interval fallback: " + std::to_wstring(s->syncIntervalFallbackSec) + L"s\n"
        L"WebSocket: " + std::wstring(s->webSocketEnabled ? L"on" : L"off") + L"\n"
        L"Debug logging: " + std::wstring(s->debugLogging ? L"on" : L"off") + L"\n\n"
        L"Pause/resume and first-run options are in the plugin menu.";
    MessageBoxW(parent, msg.c_str(), L"Notepad++ Sync — Settings", MB_OK | MB_ICONINFORMATION);
}

// ---- First run ----

void Dialogs::showFirstRunWizard(HWND parent, SyncEngine& engine) {
    int r = MessageBoxW(parent,
        L"Welcome to Notepad++ Sync!\n\n"
        L"Setup takes a minute:\n"
        L"  1. Create an account or sign in\n"
        L"  2. Encryption keys are generated on this device (never uploaded)\n"
        L"  3. Name this device\n"
        L"  4. Choose files/folders to sync\n\n"
        L"Continue?",
        L"Notepad++ Sync — Setup", MB_YESNO | MB_ICONQUESTION);
    if (r != IDYES) return;

    if (!showSignIn(parent, engine)) return;

    if (!engine.hasMasterKey()) {
        engine.generateMasterKeyIfNeeded();
        std::string rk = engine.exportRecoveryKeyWrapped();
        if (!rk.empty()) {
            std::wstring msg =
                L"Your recovery key (store it offline, safely):\n\n" + widen(rk) +
                L"\n\nIf you lose every device AND this key, your notes cannot be recovered. "
                L"There is no password reset for encrypted data.";
            MessageBoxW(parent, msg.c_str(), L"Notepad++ Sync — Recovery Key",
                        MB_OK | MB_ICONWARNING);
        }
    }
    MessageBoxW(parent,
        L"Setup complete. Use 'Synced Files/Folders' to choose what to sync — "
        L"then just use Notepad++ normally.",
        L"Notepad++ Sync", MB_OK | MB_ICONINFORMATION);
}

void Dialogs::showAbout(HWND parent) {
    MessageBoxW(parent,
        L"Notepad++ Sync 1.0.0\n\n"
        L"End-to-end encrypted file sync for Notepad++.\n"
        L"Protocol version 1. No cloud required.\n\n"
        L"https://github.com/your-org/notepadpp-sync",
        L"About Notepad++ Sync", MB_OK | MB_ICONINFORMATION);
}

} // namespace npsync
