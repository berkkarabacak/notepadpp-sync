// Dialogs.cpp — native Win32 dialog implementations. Plain and small by
// design: standard controls, no custom chrome, system fonts.
#include "Dialogs.h"

#include "Logger.h"
#include "Notepad_plus_msgs.h"
#include "SyncEngine.h"
#include "core/PathUtil.h"

#include <commctrl.h>
#include <commdlg.h>
#include <cwchar>
#include <objbase.h>
#include <shlobj.h>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace npsync {

extern HWND nppHandle(); // from PluginDefinition.cpp

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

void setText(HWND dlg, int id, const std::wstring& s) { SetDlgItemTextW(dlg, id, s.c_str()); }
void setText(HWND dlg, int id, const std::string& s) { setText(dlg, id, widen(s)); }

HWND makeLabel(HWND dlg, const wchar_t* text, int x, int y, int w, int h, int id = 0) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                           x, y, w, h, dlg, (HMENU)(intptr_t)id, nullptr, nullptr);
}

HWND makeEdit(HWND dlg, int id, int x, int y, int w, int h, DWORD extraStyle = 0,
              DWORD extraEx = WS_EX_CLIENTEDGE) {
    return CreateWindowExW(extraEx, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extraStyle,
                           x, y, w, h, dlg, (HMENU)(intptr_t)id, nullptr, nullptr);
}

HWND makeButton(HWND dlg, int id, const wchar_t* text, int x, int y, int w, int h,
                DWORD style = BS_PUSHBUTTON) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                           x, y, w, h, dlg, (HMENU)(intptr_t)id, nullptr, nullptr);
}

HWND makeCheck(HWND dlg, int id, const wchar_t* text, int x, int y, int w, bool checked) {
    HWND h = makeButton(dlg, id, text, x, y, w, 20, BS_AUTOCHECKBOX);
    SendMessageW(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return h;
}

bool checkState(HWND dlg, int id) {
    return SendMessageW(GetDlgItem(dlg, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void setDefaultFont(HWND dlg) {
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    EnumChildWindows(dlg, [](HWND child, LPARAM lp) -> BOOL {
        SendMessageW(child, WM_SETFONT, (WPARAM)lp, TRUE);
        return TRUE;
    }, (LPARAM)font);
}

// ---- generic modal dialog host (in-memory template) ----

struct DialogBase {
    SyncEngine* engine = nullptr;
    bool done = false;
    std::string result; // generic text result (prompt dialog)
};

DialogBase& ctx(HWND hwnd) {
    return *reinterpret_cast<DialogBase*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

struct DialogMemory {
    std::vector<uint8_t> buf;
    void begin(const wchar_t* title, int w, int h) {
        buf.resize(4096, 0);
        auto* d = reinterpret_cast<DLGTEMPLATE*>(buf.data());
        d->style = DS_SETFONT | DS_FIXEDSYS | WS_POPUP | WS_CAPTION | WS_SYSMENU;
        d->dwExtendedStyle = 0;
        d->cdit = 0;
        d->x = 10; d->y = 10;
        d->cx = (short)(w / 2); d->cy = (short)(h / 2);
        auto* p = reinterpret_cast<wchar_t*>(d + 1);
        *p++ = 0;
        *p++ = 0;
        wcscpy_s(p, 96, title);
        p += wcslen(p) + 1;
        *p++ = 8;
        wcscpy_s(p, 16, L"MS Shell Dlg");
    }
    DLGTEMPLATE* get() { return reinterpret_cast<DLGTEMPLATE*>(buf.data()); }
};

HINSTANCE pluginInstance() {
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&pluginInstance), &h);
    return h;
}

void ensureCommonControls() {
    static bool done = false;
    if (!done) {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES};
        InitCommonControlsEx(&icc);
        done = true;
    }
}

INT_PTR runModal(HWND parent, const wchar_t* title, int w, int h,
                 DLGPROC proc, DialogBase& base) {
    ensureCommonControls();
    DialogMemory mem;
    mem.begin(title, w, h);
    return DialogBoxIndirectParamW(pluginInstance(), mem.get(), parent, proc,
                                   reinterpret_cast<LPARAM>(&base));
}

// ---- single-line prompt dialog (rename, pairing code entry) ----

struct PromptCtx : DialogBase {
    std::wstring title, label, initial;
};

INT_PTR CALLBACK promptProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        auto& c = static_cast<PromptCtx&>(ctx(dlg));
        setText(dlg, 100, c.label);
        setText(dlg, 101, c.initial);
        SetFocus(GetDlgItem(dlg, 101));
        setDefaultFont(dlg);
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            ctx(dlg).result = narrow(editText(GetDlgItem(dlg, 101)));
            ctx(dlg).done = true;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    case WM_CLOSE: EndDialog(dlg, IDCANCEL); return TRUE;
    }
    return FALSE;
}

// Shows a one-line input. Returns false on cancel.
bool prompt(HWND parent, const std::wstring& title, const std::wstring& label,
            const std::string& initial, std::string& out) {
    PromptCtx c;
    c.label = label;
    c.initial = widen(initial);
    DialogMemory mem;
    mem.begin(title.c_str(), 360, 120);
    // Controls are created in the proc; template only provides the frame.
    INT_PTR r = DialogBoxIndirectParamW(pluginInstance(), mem.get(), parent,
        [](HWND dlg, UINT msg, WPARAM wp, LPARAM lp) -> INT_PTR {
            if (msg == WM_INITDIALOG) {
                // Create the prompt controls lazily (keeps template trivial).
                makeLabel(dlg, L"", 12, 12, 336, 20, 100);
                makeEdit(dlg, 101, 12, 36, 336, 22);
                makeButton(dlg, IDOK, L"OK", 168, 72, 84, 26, BS_DEFPUSHBUTTON);
                makeButton(dlg, IDCANCEL, L"Cancel", 262, 72, 84, 26);
            }
            return promptProc(dlg, msg, wp, lp);
        }, reinterpret_cast<LPARAM>(&c));
    if (r == IDOK && c.done) { out = c.result; return true; }
    return false;
}

} // namespace
// ============================ Sign In ============================

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
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                        12, 140, 340, 20, dlg, (HMENU)(intptr_t)ID_SIGNIN_STATUS, nullptr, nullptr);
        setDefaultFont(dlg);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SIGNIN_OK: {
            std::string email = narrow(editText(GetDlgItem(dlg, ID_SIGNIN_EMAIL)));
            std::string password = narrow(editText(GetDlgItem(dlg, ID_SIGNIN_PASSWORD)));
            bool create = SendMessageW(GetDlgItem(dlg, ID_SIGNIN_CREATE), BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (email.empty() || password.empty()) {
                setText(dlg, ID_SIGNIN_STATUS, L"Email and password are required.");
                return TRUE;
            }
            std::string err;
            if (ctx(dlg).engine->signIn(email, password, err, create)) {
                ctx(dlg).done = true;
                EndDialog(dlg, IDOK);
            } else {
                setText(dlg, ID_SIGNIN_STATUS, widen(err));
            }
            return TRUE;
        }
        case ID_SIGNIN_CANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}
} // namespace

bool Dialogs::showSignIn(HWND parent, SyncEngine& engine) {
    DialogBase base{&engine, false, ""};
    INT_PTR r = runModal(parent, L"Notepad++ Sync — Sign In", 370, 175, signInProc, base);
    return r == IDOK && base.done;
}

// ============================ Status ============================

namespace {
enum { ID_ST_TEXT = 100, ID_ST_REFRESH, ID_ST_DEVICES };

void fillStatus(HWND dlg, SyncEngine& e) {
    StatusInfo st = e.status();
    wchar_t buf[640];
    swprintf(buf, 640,
        L"Status: %hs\r\nLast sync: %hs\r\n\r\nFiles synchronized: %d\r\n"
        L"Pending uploads: %d\r\nPending downloads: %d\r\nConflicts: %d",
        st.statusText.c_str(),
        st.lastSyncTime.empty() ? "never" : st.lastSyncTime.c_str(),
        st.filesSynchronized, st.pendingUploads, st.pendingDownloads, st.conflicts);
    setText(dlg, ID_ST_TEXT, buf);

    std::vector<SyncEngine::DeviceInfo> devs;
    std::string err;
    std::wstring lines;
    if (e.listDevices(devs, err)) {
        for (auto& d : devs) {
            if (d.revoked) continue;
            lines += d.current ? L"• " : L"  ";
            lines += widen(d.name);
            lines += d.current ? L"  (this device)" : L"";
            lines += L"\r\n";
        }
    }
    if (lines.empty()) lines = L"(device list unavailable offline)";
    setText(dlg, ID_ST_DEVICES, lines);
}

INT_PTR CALLBACK statusProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        makeLabel(dlg, L"Notepad++ Sync", 12, 10, 200, 20);
        makeEdit(dlg, ID_ST_TEXT, 12, 34, 330, 120,
                 ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0);
        makeLabel(dlg, L"Devices:", 12, 162, 200, 18);
        makeEdit(dlg, ID_ST_DEVICES, 12, 182, 330, 90, ES_MULTILINE | ES_READONLY, 0);
        makeButton(dlg, ID_ST_REFRESH, L"Refresh", 128, 284, 100, 26);
        makeButton(dlg, IDOK, L"Close", 238, 284, 100, 26, BS_DEFPUSHBUTTON);
        setDefaultFont(dlg);
        fillStatus(dlg, *ctx(dlg).engine);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_ST_REFRESH) { fillStatus(dlg, *ctx(dlg).engine); return TRUE; }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDOK); return TRUE; }
        break;
    case WM_CLOSE: EndDialog(dlg, IDOK); return TRUE;
    }
    return FALSE;
}
} // namespace

void Dialogs::showStatus(HWND parent, SyncEngine& engine) {
    DialogBase base{&engine, false, ""};
    runModal(parent, L"Notepad++ Sync — Status", 360, 325, statusProc, base);
}

// ============================ Devices ============================

namespace {
enum { ID_DEV_LIST = 100, ID_DEV_REFRESH, ID_DEV_RENAME, ID_DEV_REVOKE,
       ID_DEV_PAIR, ID_DEV_APPROVE, ID_DEV_MSG };

struct DevicesCtx : DialogBase {
    std::vector<SyncEngine::DeviceInfo> devs;
};

void fillDevices(HWND dlg, DevicesCtx& c) {
    HWND lv = GetDlgItem(dlg, ID_DEV_LIST);
    ListView_DeleteAllItems(lv);
    std::string err;
    if (!c.engine->listDevices(c.devs, err)) {
        setText(dlg, ID_DEV_MSG, widen(err));
        return;
    }
    setText(dlg, ID_DEV_MSG, L"");
    int row = 0;
    for (auto& d : c.devs) {
        std::wstring name = widen(d.name);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = name.data();
        ListView_InsertItem(lv, &item);
        std::wstring id = widen(d.id);
        ListView_SetItemText(lv, row, 1, id.data());
        std::wstring lastSeen = widen(d.lastSeenAt.substr(0, 19));
        ListView_SetItemText(lv, row, 2, lastSeen.data());
        std::wstring status = d.revoked ? L"revoked" : (d.current ? L"this device" : L"active");
        ListView_SetItemText(lv, row, 3, status.data());
        ++row;
    }
}

INT_PTR CALLBACK devicesProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        DevicesCtx& c = static_cast<DevicesCtx&>(ctx(dlg));
        HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_TABSTOP,
            12, 12, 480, 200, dlg, (HMENU)(intptr_t)ID_DEV_LIST, nullptr, nullptr);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT);
        const wchar_t* cols[] = {L"Name", L"Device ID", L"Last seen", L"Status"};
        int widths[] = {150, 150, 100, 80};
        for (int i = 0; i < 4; ++i) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = const_cast<wchar_t*>(cols[i]);
            col.cx = widths[i];
            ListView_InsertColumn(lv, i, &col);
        }
        makeButton(dlg, ID_DEV_REFRESH, L"Refresh", 12, 222, 90, 26);
        makeButton(dlg, ID_DEV_RENAME, L"Rename", 110, 222, 90, 26);
        makeButton(dlg, ID_DEV_REVOKE, L"Revoke", 208, 222, 90, 26);
        makeButton(dlg, ID_DEV_PAIR, L"Pair new device", 306, 222, 120, 26);
        makeButton(dlg, ID_DEV_APPROVE, L"Approve pairing", 12, 256, 120, 26);
        makeButton(dlg, IDOK, L"Close", 402, 256, 90, 26, BS_DEFPUSHBUTTON);
        makeLabel(dlg, L"", 12, 292, 480, 18, ID_DEV_MSG);
        setDefaultFont(dlg);
        fillDevices(dlg, c);
        return TRUE;
    }
    case WM_COMMAND: {
        DevicesCtx& c = static_cast<DevicesCtx&>(ctx(dlg));
        int sel = ListView_GetNextItem(GetDlgItem(dlg, ID_DEV_LIST), -1, LVNI_SELECTED);
        switch (LOWORD(wp)) {
        case ID_DEV_REFRESH: fillDevices(dlg, c); return TRUE;
        case ID_DEV_RENAME: {
            if (sel < 0 || sel >= (int)c.devs.size()) return TRUE;
            std::string name;
            if (prompt(dlg, L"Rename device", L"New name:", c.devs[sel].name, name)) {
                std::string err;
                if (!c.engine->renameDeviceById(c.devs[sel].id, name, err))
                    setText(dlg, ID_DEV_MSG, widen(err));
                fillDevices(dlg, c);
            }
            return TRUE;
        }
        case ID_DEV_REVOKE: {
            if (sel < 0 || sel >= (int)c.devs.size()) return TRUE;
            if (c.devs[sel].current) {
                setText(dlg, ID_DEV_MSG, L"Use Sign Out to remove this device.");
                return TRUE;
            }
            std::wstring q = L"Revoke device \"" + widen(c.devs[sel].name) +
                L"\"? It will be signed out immediately.";
            if (MessageBoxW(dlg, q.c_str(), L"Notepad++ Sync", MB_YESNO | MB_ICONWARNING) == IDYES) {
                std::string err;
                if (!c.engine->revokeDeviceById(c.devs[sel].id, err))
                    setText(dlg, ID_DEV_MSG, widen(err));
                fillDevices(dlg, c);
            }
            return TRUE;
        }
        case ID_DEV_PAIR: {
            // This device joins: request a code for another device to approve.
            std::string code, err;
            if (c.engine->pairNewDevice(code, err)) {
                std::wstring m = L"Pairing code (valid 5 minutes):\n\n    " + widen(code) +
                    L"\n\nOn a device that already has your notes, open Manage Devices → "
                    L"Approve pairing and enter this code.";
                MessageBoxW(dlg, m.c_str(), L"Notepad++ Sync — Pairing", MB_OK | MB_ICONINFORMATION);
            } else {
                setText(dlg, ID_DEV_MSG, widen(err));
            }
            return TRUE;
        }
        case ID_DEV_APPROVE: {
            std::string code;
            if (prompt(dlg, L"Approve pairing", L"Enter code shown on the new device (ABCD-EFGH):", "", code)) {
                std::string err;
                if (c.engine->approvePairing(code, err))
                    setText(dlg, ID_DEV_MSG, L"Approved. The new device can now unlock its keys.");
                else
                    setText(dlg, ID_DEV_MSG, widen(err));
            }
            return TRUE;
        }
        case IDOK:
        case IDCANCEL:
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        break;
    }
    case WM_CLOSE: EndDialog(dlg, IDOK); return TRUE;
    }
    return FALSE;
}
} // namespace

void Dialogs::showDevices(HWND parent, SyncEngine& engine) {
    ensureCommonControls();
    DevicesCtx c;
    c.engine = &engine;
    DialogMemory mem;
    mem.begin(L"Notepad++ Sync — Manage Devices", 510, 325);
    DialogBoxIndirectParamW(pluginInstance(), mem.get(), parent, devicesProc,
                            reinterpret_cast<LPARAM>(&c));
}
// ============================ Synced Files/Folders ============================

namespace {
enum { ID_SF_LIST = 100, ID_SF_ADDFOLDER, ID_SF_ADDFILE, ID_SF_REMOVE,
       ID_SF_IGNORE, ID_SF_SAVEIGNORE, ID_SF_MSG };

struct SyncedFilesCtx : DialogBase {
    std::vector<std::pair<std::string, std::wstring>> rows; // (id, path)
    std::vector<bool> isFolder;
};

void fillSyncedFiles(HWND dlg, SyncedFilesCtx& c) {
    HWND lv = GetDlgItem(dlg, ID_SF_LIST);
    ListView_DeleteAllItems(lv);
    c.rows.clear();
    c.isFolder.clear();
    for (auto& r : c.engine->db()->listSyncRoots()) { c.rows.push_back(r); c.isFolder.push_back(true); }
    for (auto& r : c.engine->db()->listSyncFiles()) { c.rows.push_back(r); c.isFolder.push_back(false); }
    int row = 0;
    for (auto& [id, path] : c.rows) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        std::wstring type = c.isFolder[row] ? L"Folder" : L"File";
        item.pszText = type.data();
        ListView_InsertItem(lv, &item);
        ListView_SetItemText(lv, row, 1, const_cast<wchar_t*>(path.c_str()));
        ++row;
    }
}

INT_PTR CALLBACK syncedFilesProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        SyncedFilesCtx& c = static_cast<SyncedFilesCtx&>(ctx(dlg));
        HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_TABSTOP,
            12, 12, 500, 170, dlg, (HMENU)(intptr_t)ID_SF_LIST, nullptr, nullptr);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t*>(L"Type"); col.cx = 70;
        ListView_InsertColumn(lv, 0, &col);
        col.pszText = const_cast<wchar_t*>(L"Path"); col.cx = 420;
        ListView_InsertColumn(lv, 1, &col);

        makeButton(dlg, ID_SF_ADDFOLDER, L"Add Folder…", 12, 190, 110, 26);
        makeButton(dlg, ID_SF_ADDFILE, L"Add File…", 130, 190, 110, 26);
        makeButton(dlg, ID_SF_REMOVE, L"Remove Selected", 248, 190, 130, 26);

        makeLabel(dlg, L"Ignore patterns (one per line, .gitignore-style; a .npsyncignore file in a synced folder also applies):",
                  12, 226, 500, 30);
        makeEdit(dlg, ID_SF_IGNORE, 12, 258, 500, 90,
                 ES_MULTILINE | WS_VSCROLL | ES_WANTRETURN, WS_EX_CLIENTEDGE);
        makeButton(dlg, ID_SF_SAVEIGNORE, L"Save Patterns", 12, 356, 110, 26);
        makeButton(dlg, IDOK, L"Close", 422, 356, 90, 26, BS_DEFPUSHBUTTON);
        makeLabel(dlg, L"", 130, 360, 280, 18, ID_SF_MSG);
        setDefaultFont(dlg);

        std::wstring patterns;
        for (auto& p : c.engine->settings()->extraIgnorePatterns) {
            patterns += widen(p);
            patterns += L"\r\n";
        }
        setText(dlg, ID_SF_IGNORE, patterns);
        fillSyncedFiles(dlg, c);
        return TRUE;
    }
    case WM_COMMAND: {
        SyncedFilesCtx& c = static_cast<SyncedFilesCtx&>(ctx(dlg));
        switch (LOWORD(wp)) {
        case ID_SF_ADDFOLDER: {
            (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            wchar_t path[MAX_PATH] = {0};
            BROWSEINFOW bi{};
            bi.hwndOwner = dlg;
            bi.lpszTitle = L"Choose a folder to synchronize";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            if (LPITEMIDLIST pidl = SHBrowseForFolderW(&bi)) {
                if (SHGetPathFromIDListW(pidl, path)) {
                    c.engine->addSyncRootPath(path, true);
                    setText(dlg, ID_SF_MSG, L"Folder added.");
                }
                CoTaskMemFree(pidl);
            }
            fillSyncedFiles(dlg, c);
            return TRUE;
        }
        case ID_SF_ADDFILE: {
            (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            wchar_t path[MAX_PATH] = {0};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = dlg;
            ofn.lpstrFile = path;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"Choose a file to synchronize";
            ofn.Flags = OFN_FILEMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                c.engine->addSyncRootPath(path, false);
                setText(dlg, ID_SF_MSG, L"File added.");
            }
            fillSyncedFiles(dlg, c);
            return TRUE;
        }
        case ID_SF_REMOVE: {
            int sel = ListView_GetNextItem(GetDlgItem(dlg, ID_SF_LIST), -1, LVNI_SELECTED);
            if (sel < 0 || sel >= (int)c.rows.size()) return TRUE;
            std::wstring q = L"Stop syncing\r\n" + c.rows[sel].second +
                L"\r\n\r\n(Local files are NOT deleted.)";
            if (MessageBoxW(dlg, q.c_str(), L"Notepad++ Sync", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                c.engine->removeSyncRootPath(c.rows[sel].first);
                fillSyncedFiles(dlg, c);
            }
            return TRUE;
        }
        case ID_SF_SAVEIGNORE: {
            std::wstring raw = editText(GetDlgItem(dlg, ID_SF_IGNORE));
            std::vector<std::string> pats;
            std::wstring line;
            for (wchar_t ch : raw) {
                if (ch == L'\r') continue;
                if (ch == L'\n') { if (!line.empty()) pats.push_back(narrow(line)); line.clear(); }
                else line.push_back(ch);
            }
            if (!line.empty()) pats.push_back(narrow(line));
            c.engine->settings()->extraIgnorePatterns = pats;
            c.engine->saveSettings();
            c.engine->reloadRoots();
            setText(dlg, ID_SF_MSG, L"Patterns saved.");
            return TRUE;
        }
        case IDOK:
        case IDCANCEL:
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        break;
    }
    case WM_CLOSE: EndDialog(dlg, IDOK); return TRUE;
    }
    return FALSE;
}
} // namespace

void Dialogs::showSyncedFiles(HWND parent, SyncEngine& engine) {
    ensureCommonControls();
    SyncedFilesCtx c;
    c.engine = &engine;
    DialogMemory mem;
    mem.begin(L"Notepad++ Sync — Synced Files/Folders", 530, 400);
    DialogBoxIndirectParamW(pluginInstance(), mem.get(), parent, syncedFilesProc,
                            reinterpret_cast<LPARAM>(&c));
}

// ============================ Conflicts ============================

namespace {
enum { ID_CF_LIST = 100, ID_CF_LOCAL, ID_CF_REMOTE, ID_CF_BOTH, ID_CF_COMPARE,
       ID_CF_REFRESH, ID_CF_MSG };

struct ConflictsCtx : DialogBase {
    std::vector<ConflictState> items;
};

void fillConflicts(HWND dlg, ConflictsCtx& c) {
    HWND lv = GetDlgItem(dlg, ID_CF_LIST);
    ListView_DeleteAllItems(lv);
    c.items = c.engine->db()->conflicts();
    int row = 0;
    for (auto& item : c.items) {
        LVITEMW lvi{};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = row;
        std::wstring rel = widen(item.relPath);
        lvi.pszText = rel.data();
        ListView_InsertItem(lv, &lvi);
        wchar_t ver[32];
        swprintf(ver, 32, L"remote v%lld", (long long)item.remoteVersion);
        ListView_SetItemText(lv, row, 1, ver);
        std::wstring when = std::to_wstring(item.detectedAt);
        ListView_SetItemText(lv, row, 2, when.data());
        ++row;
    }
    setText(dlg, ID_CF_MSG, c.items.empty() ? L"No conflicts." : L"");
}

INT_PTR CALLBACK conflictsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        ConflictsCtx& c = static_cast<ConflictsCtx&>(ctx(dlg));
        HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_TABSTOP,
            12, 12, 520, 180, dlg, (HMENU)(intptr_t)ID_CF_LIST, nullptr, nullptr);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT);
        const wchar_t* cols[] = {L"File", L"Version", L"Detected"};
        int widths[] = {330, 90, 90};
        for (int i = 0; i < 3; ++i) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = const_cast<wchar_t*>(cols[i]);
            col.cx = widths[i];
            ListView_InsertColumn(lv, i, &col);
        }
        makeLabel(dlg, L"Both versions are always preserved. Choose a resolution:", 12, 200, 520, 18);
        makeButton(dlg, ID_CF_LOCAL, L"Keep Local", 12, 224, 100, 26);
        makeButton(dlg, ID_CF_REMOTE, L"Keep Remote", 120, 224, 100, 26);
        makeButton(dlg, ID_CF_BOTH, L"Keep Both", 228, 224, 100, 26);
        makeButton(dlg, ID_CF_COMPARE, L"Open Comparison", 336, 224, 130, 26);
        makeButton(dlg, ID_CF_REFRESH, L"Refresh", 12, 258, 90, 26);
        makeButton(dlg, IDOK, L"Close", 442, 258, 90, 26, BS_DEFPUSHBUTTON);
        makeLabel(dlg, L"", 110, 262, 320, 18, ID_CF_MSG);
        setDefaultFont(dlg);
        fillConflicts(dlg, c);
        return TRUE;
    }
    case WM_COMMAND: {
        ConflictsCtx& c = static_cast<ConflictsCtx&>(ctx(dlg));
        int sel = ListView_GetNextItem(GetDlgItem(dlg, ID_CF_LIST), -1, LVNI_SELECTED);
        auto resolve = [&](const char* strategy) {
            if (sel < 0 || sel >= (int)c.items.size()) return;
            std::string err;
            if (c.engine->resolveConflict(c.items[sel].fileId, strategy, err))
                setText(dlg, ID_CF_MSG, L"Resolved.");
            else
                setText(dlg, ID_CF_MSG, widen(err));
            fillConflicts(dlg, c);
        };
        switch (LOWORD(wp)) {
        case ID_CF_LOCAL:  resolve("keepLocal");  return TRUE;
        case ID_CF_REMOTE: resolve("keepRemote"); return TRUE;
        case ID_CF_BOTH:   resolve("keepBoth");   return TRUE;
        case ID_CF_COMPARE: {
            if (sel < 0 || sel >= (int)c.items.size()) return TRUE;
            // Open the preserved local copy and the canonical file side by side.
            std::wstring localCopy = widen(c.items[sel].localCopyPath);
            HWND npp = nppHandle();
            if (npp && !localCopy.empty())
                ::SendMessageW(npp, NPPM_DOOPEN, 0, (LPARAM)localCopy.c_str());
            return TRUE;
        }
        case ID_CF_REFRESH: fillConflicts(dlg, c); return TRUE;
        case IDOK:
        case IDCANCEL:
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        break;
    }
    case WM_CLOSE: EndDialog(dlg, IDOK); return TRUE;
    }
    return FALSE;
}
} // namespace

void Dialogs::showConflicts(HWND parent, SyncEngine& engine) {
    ensureCommonControls();
    ConflictsCtx c;
    c.engine = &engine;
    DialogMemory mem;
    mem.begin(L"Notepad++ Sync — Conflicts", 550, 300);
    DialogBoxIndirectParamW(pluginInstance(), mem.get(), parent, conflictsProc,
                            reinterpret_cast<LPARAM>(&c));
}
// ============================ Settings (tabbed) ============================

namespace {
enum {
    ID_SET_TAB = 100, ID_SET_OK, ID_SET_CANCEL, ID_SET_MSG,
    // General
    ID_G_AUTOSTART = 200, ID_G_PAUSE, ID_G_INTERVAL, ID_G_WS, ID_G_NOTIFY,
    // Files
    ID_F_MAXSIZE, ID_F_IGNORE,
    // Session
    ID_S_FILESONLY, ID_S_TABS, ID_S_CURSOR, ID_S_UNSAVED,
    // Security
    ID_SEC_DEVNAME, ID_SEC_RECOVERY, ID_SEC_DEVICES,
    // Advanced
    ID_A_URL, ID_A_DEBUG, ID_A_DBLOC, ID_A_RESET,
};

struct SettingsCtx : DialogBase {
    std::vector<std::vector<int>> tabControls; // control IDs per tab
};

void settingsShowTab(HWND dlg, SettingsCtx& c, int tab) {
    for (size_t t = 0; t < c.tabControls.size(); ++t)
        for (int id : c.tabControls[t])
            if (HWND h = GetDlgItem(dlg, id))
                ShowWindow(h, (int)t == tab ? SW_SHOW : SW_HIDE);
}

INT_PTR CALLBACK settingsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        SettingsCtx& c = static_cast<SettingsCtx&>(ctx(dlg));
        Settings* s = c.engine ? c.engine->settings() : nullptr;
        HWND tab = CreateWindowExW(0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 8, 8, 480, 26, dlg,
            (HMENU)(intptr_t)ID_SET_TAB, nullptr, nullptr);
        const wchar_t* tabs[] = {L"General", L"Files", L"Session", L"Security", L"Advanced"};
        for (int i = 0; i < 5; ++i) {
            TCITEMW ti{};
            ti.mask = TCIF_TEXT;
            ti.pszText = const_cast<wchar_t*>(tabs[i]);
            TabCtrl_InsertItem(tab, i, &ti);
        }
        const int tx = 16, ty = 44; // tab content origin

        // General
        makeCheck(dlg, ID_G_AUTOSTART, L"Start sync automatically", tx, ty, 220, s->startSyncAutomatically);
        makeCheck(dlg, ID_G_PAUSE, L"Pause sync", tx, ty + 26, 220, s->pauseSync);
        makeCheck(dlg, ID_G_WS, L"Realtime connection (WebSocket)", tx, ty + 52, 260, s->webSocketEnabled);
        makeCheck(dlg, ID_G_NOTIFY, L"Notifications", tx, ty + 78, 220, s->notificationsEnabled);
        makeLabel(dlg, L"Sync interval fallback (sec):", tx, ty + 108, 200, 18, 219);
        makeEdit(dlg, ID_G_INTERVAL, tx + 210, ty + 106, 60, 22);
        setText(dlg, ID_G_INTERVAL, std::to_string(s->syncIntervalFallbackSec));

        // Files
        makeLabel(dlg, L"Max file size (MB):", tx, ty, 200, 18, 238);
        makeEdit(dlg, ID_F_MAXSIZE, tx + 210, ty - 2, 60, 22);
        setText(dlg, ID_F_MAXSIZE, std::to_string(s->maxFileBytes / 1024 / 1024));
        makeLabel(dlg, L"Manage synced folders/files with 'Synced Files/Folders' in the plugin menu.",
                  tx, ty + 32, 460, 18, 239);
        makeLabel(dlg, L"Ignore patterns:", tx, ty + 58, 200, 18, 237);
        makeEdit(dlg, ID_F_IGNORE, tx, ty + 78, 456, 110,
                 ES_MULTILINE | WS_VSCROLL | ES_WANTRETURN, WS_EX_CLIENTEDGE);
        {
            std::wstring pats;
            for (auto& p : s->extraIgnorePatterns) { pats += widen(p); pats += L"\r\n"; }
            setText(dlg, ID_F_IGNORE, pats);
        }

        // Session
        makeButton(dlg, ID_S_FILESONLY, L"Sync files only", tx, ty, 220, 20, BS_AUTORADIOBUTTON | WS_GROUP);
        makeButton(dlg, ID_S_TABS, L"Sync files + open tabs", tx, ty + 26, 220, 20, BS_AUTORADIOBUTTON);
        makeButton(dlg, ID_S_CURSOR, L"Sync files + tabs + cursor positions", tx, ty + 52, 280, 20, BS_AUTORADIOBUTTON);
        CheckRadioButton(dlg, ID_S_FILESONLY, ID_S_CURSOR,
            ID_S_FILESONLY + (int)s->sessionMode);
        makeCheck(dlg, ID_S_UNSAVED,
            L"Sync unsaved notes (WARNING: uploads never-saved scratch content)", tx, ty + 86, 460,
            s->syncUnsavedNotes);

        // Security
        makeLabel(dlg, L"This device name:", tx, ty, 200, 18, 279);
        makeEdit(dlg, ID_SEC_DEVNAME, tx + 210, ty - 2, 200, 22);
        setText(dlg, ID_SEC_DEVNAME, s->deviceName);
        makeButton(dlg, ID_SEC_RECOVERY, L"Show recovery key", tx, ty + 34, 160, 26);
        makeButton(dlg, ID_SEC_DEVICES, L"Manage devices…", tx, ty + 68, 160, 26);

        // Advanced
        makeLabel(dlg, L"Backend URL:", tx, ty, 200, 18, 298);
        makeEdit(dlg, ID_A_URL, tx, ty + 20, 456, 22);
        setText(dlg, ID_A_URL, s->backendUrl);
        makeCheck(dlg, ID_A_DEBUG, L"Debug logging", tx, ty + 54, 220, s->debugLogging);
        makeLabel(dlg, L"Database location (blank = default):", tx, ty + 84, 300, 18, 297);
        makeEdit(dlg, ID_A_DBLOC, tx, ty + 104, 456, 22);
        setText(dlg, ID_A_DBLOC, s->databaseLocation);
        makeButton(dlg, ID_A_RESET, L"Reset local sync state…", tx, ty + 140, 180, 26);

        makeButton(dlg, ID_SET_OK, L"Save", 300, 250, 90, 26, BS_DEFPUSHBUTTON);
        makeButton(dlg, ID_SET_CANCEL, L"Cancel", 398, 250, 90, 26);
        makeLabel(dlg, L"", 12, 254, 280, 18, ID_SET_MSG);

        c.tabControls = {
            {ID_G_AUTOSTART, ID_G_PAUSE, ID_G_WS, ID_G_NOTIFY, ID_G_INTERVAL, 219},
            {ID_F_MAXSIZE, ID_F_IGNORE, 237, 238, 239},
            {ID_S_FILESONLY, ID_S_TABS, ID_S_CURSOR, ID_S_UNSAVED},
            {ID_SEC_DEVNAME, ID_SEC_RECOVERY, ID_SEC_DEVICES, 279},
            {ID_A_URL, ID_A_DEBUG, ID_A_DBLOC, ID_A_RESET, 297, 298},
        };
        settingsShowTab(dlg, c, 0);
        setDefaultFont(dlg);
        return TRUE;
    }
    case WM_NOTIFY: {
        SettingsCtx& c = static_cast<SettingsCtx&>(ctx(dlg));
        NMHDR* hdr = reinterpret_cast<NMHDR*>(lp);
        if (hdr->idFrom == ID_SET_TAB && hdr->code == TCN_SELCHANGE) {
            settingsShowTab(dlg, c, TabCtrl_GetCurSel(GetDlgItem(dlg, ID_SET_TAB)));
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        SettingsCtx& c = static_cast<SettingsCtx&>(ctx(dlg));
        Settings* s = c.engine->settings();
        switch (LOWORD(wp)) {
        case ID_SEC_RECOVERY: {
            std::string rk = c.engine->exportRecoveryKeyWrapped();
            std::wstring m = rk.empty()
                ? L"No recovery key is stored on this device."
                : L"Your recovery key (keep it offline and private):\n\n" + widen(rk) +
                  L"\n\nLosing every device AND this key makes your notes unrecoverable.";
            MessageBoxW(dlg, m.c_str(), L"Notepad++ Sync — Recovery Key",
                        MB_OK | (rk.empty() ? MB_ICONINFORMATION : MB_ICONWARNING));
            return TRUE;
        }
        case ID_SEC_DEVICES:
            Dialogs::showDevices(dlg, *c.engine);
            return TRUE;
        case ID_A_RESET:
            if (MessageBoxW(dlg,
                    L"Reset local sync state? The local database, queue, and shadow copies "
                    L"are cleared; files on disk are kept. The next sync reconciles with the server.",
                    L"Notepad++ Sync", MB_YESNO | MB_ICONWARNING) == IDYES) {
                setText(dlg, ID_SET_MSG, L"Restart Notepad++ to complete the reset.");
                // Deletion happens on next start if the flag is set.
                c.engine->settings()->databaseLocation = L"";
                c.engine->saveSettings();
            }
            return TRUE;
        case ID_SET_OK: {
            s->startSyncAutomatically = checkState(dlg, ID_G_AUTOSTART);
            s->webSocketEnabled = checkState(dlg, ID_G_WS);
            s->notificationsEnabled = checkState(dlg, ID_G_NOTIFY);
            try { s->syncIntervalFallbackSec = std::max(5, std::stoi(narrow(editText(GetDlgItem(dlg, ID_G_INTERVAL))))); }
            catch (...) {}
            try { s->maxFileBytes = (int64_t)std::max(1, std::stoi(narrow(editText(GetDlgItem(dlg, ID_F_MAXSIZE))))) * 1024 * 1024; }
            catch (...) {}
            {
                std::wstring raw = editText(GetDlgItem(dlg, ID_F_IGNORE));
                std::vector<std::string> pats;
                std::wstring line;
                for (wchar_t ch : raw) {
                    if (ch == L'\r') continue;
                    if (ch == L'\n') { if (!line.empty()) pats.push_back(narrow(line)); line.clear(); }
                    else line.push_back(ch);
                }
                if (!line.empty()) pats.push_back(narrow(line));
                s->extraIgnorePatterns = pats;
            }
            if (checkState(dlg, ID_S_FILESONLY)) s->sessionMode = SessionSyncMode::FilesOnly;
            else if (checkState(dlg, ID_S_TABS)) s->sessionMode = SessionSyncMode::FilesAndTabs;
            else s->sessionMode = SessionSyncMode::FilesTabsCursor;
            s->syncUnsavedNotes = checkState(dlg, ID_S_UNSAVED);
            s->deviceName = narrow(editText(GetDlgItem(dlg, ID_SEC_DEVNAME)));
            std::string url = narrow(editText(GetDlgItem(dlg, ID_A_URL)));
            if (!url.empty()) s->backendUrl = url;
            s->debugLogging = checkState(dlg, ID_A_DEBUG);
            s->databaseLocation = editText(GetDlgItem(dlg, ID_A_DBLOC));
            bool newPause = checkState(dlg, ID_G_PAUSE);
            c.engine->saveSettings();
            c.engine->setPaused(newPause);
            c.engine->reloadRoots();
            Logger::setLevel(s->debugLogging ? LogLevel::Debug : LogLevel::Info);
            c.done = true;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case ID_SET_CANCEL:
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
} // namespace

void Dialogs::showSettings(HWND parent, SyncEngine& engine) {
    ensureCommonControls();
    SettingsCtx c;
    c.engine = &engine;
    DialogMemory mem;
    mem.begin(L"Notepad++ Sync — Settings", 500, 295);
    DialogBoxIndirectParamW(pluginInstance(), mem.get(), parent, settingsProc,
                            reinterpret_cast<LPARAM>(&c));
}

// ============================ First run & About ============================

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
    // Let the user name the device right away.
    std::string name;
    if (prompt(parent, L"Name this device", L"Device name (e.g. Laptop-Home):",
               engine.settings()->deviceName, name)) {
        engine.settings()->deviceName = name;
        engine.saveSettings();
    }
    Dialogs::showSyncedFiles(parent, engine);
    MessageBoxW(parent,
        L"Setup complete. Sync now runs in the background — just use Notepad++.",
        L"Notepad++ Sync", MB_OK | MB_ICONINFORMATION);
}

void Dialogs::showAbout(HWND parent) {
    MessageBoxW(parent,
        L"Notepad++ Sync 1.0.0\n\n"
        L"End-to-end encrypted file sync for Notepad++.\n"
        L"Protocol version 1. No cloud required.\n\n"
        L"https://github.com/berkkarabacak/notepadpp-sync",
        L"About Notepad++ Sync", MB_OK | MB_ICONINFORMATION);
}

} // namespace npsync
