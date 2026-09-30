// Dialogs.cpp — native Win32 dialog implementations. Plain and small by
// design: standard controls, no custom chrome, system fonts.
#include "Dialogs.h"

#include "DialogLayout.h"
#include "Logger.h"
#include "Notepad_plus_msgs.h"
#include "SyncEngine.h"
#include "core/PathUtil.h"

#include <commctrl.h>
#include <commdlg.h>
#include <cwchar>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace npsync
{

extern HWND nppHandle(); // from PluginDefinition.cpp

namespace
{

std::wstring widen(const std::string& s) {
    return PathUtil::utf8ToWide(s);
}
std::string narrow(const std::wstring& s) {
    return PathUtil::wideToUtf8(s);
}

std::wstring editText(HWND hEdit) {
    int len = GetWindowTextLengthW(hEdit);
    std::wstring out(len + 1, L'\0');
    GetWindowTextW(hEdit, out.data(), len + 1);
    out.resize(len);
    return out;
}

void setText(HWND dlg, int id, const std::wstring& s) {
    SetDlgItemTextW(dlg, id, s.c_str());
}
void setText(HWND dlg, int id, const std::string& s) {
    setText(dlg, id, widen(s));
}

HWND makeLabel(HWND dlg, const wchar_t* text, int x, int y, int w, int h, int id = 0) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, dlg, (HMENU)(intptr_t)id,
                           nullptr, nullptr);
}

HWND makeEdit(HWND dlg, int id, int x, int y, int w, int h, DWORD extraStyle = 0,
              DWORD extraEx = WS_EX_CLIENTEDGE) {
    return CreateWindowExW(extraEx, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extraStyle, x, y, w, h, dlg,
                           (HMENU)(intptr_t)id, nullptr, nullptr);
}

HWND makeButton(HWND dlg, int id, const wchar_t* text, int x, int y, int w, int h,
                DWORD style = BS_PUSHBUTTON) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, x, y, w, h, dlg,
                           (HMENU)(intptr_t)id, nullptr, nullptr);
}

HWND makeCheck(HWND dlg, int id, const wchar_t* text, int x, int y, int w, bool checked) {
    HWND h = makeButton(dlg, id, text, x, y, w, layout::kCheckHeight, BS_AUTOCHECKBOX);
    SendMessageW(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return h;
}

bool checkState(HWND dlg, int id) {
    return SendMessageW(GetDlgItem(dlg, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void setDefaultFont(HWND dlg) {
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    EnumChildWindows(
        dlg,
        [](HWND child, LPARAM lp) -> BOOL {
            SendMessageW(child, WM_SETFONT, (WPARAM)lp, TRUE);
            return TRUE;
        },
        (LPARAM)font);
}

// ---- generic modal dialog host (in-memory template) ----

struct DialogBase
{
    SyncEngine* engine = nullptr;
    bool done = false;
    std::string result; // generic text result (prompt dialog)
};

DialogBase& ctx(HWND hwnd) {
    return *reinterpret_cast<DialogBase*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

struct DialogMemory
{
    std::vector<uint8_t> buf;
    void begin(const wchar_t* title, int w, int h) {
        buf.resize(4096, 0);
        auto* d = reinterpret_cast<DLGTEMPLATE*>(buf.data());
        d->style = DS_SETFONT | DS_FIXEDSYS | WS_POPUP | WS_CAPTION | WS_SYSMENU;
        d->dwExtendedStyle = 0;
        d->cdit = 0;
        d->x = 10;
        d->y = 10;
        // w and h are pixels. See DialogLayout.h for the dialog-unit conversion.
        d->cx = (short)layout::toDlgX(w);
        d->cy = (short)layout::toDlgY(h);
        auto* p = reinterpret_cast<wchar_t*>(d + 1);
        *p++ = 0;
        *p++ = 0;
        wcscpy_s(p, 96, title);
        p += wcslen(p) + 1;
        *p++ = 8;
        wcscpy_s(p, 16, L"MS Shell Dlg");
    }
    DLGTEMPLATE* get() {
        return reinterpret_cast<DLGTEMPLATE*>(buf.data());
    }
};

HINSTANCE pluginInstance() {
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
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

INT_PTR runModal(HWND parent, const wchar_t* title, int w, int h, DLGPROC proc, DialogBase& base) {
    ensureCommonControls();
    DialogMemory mem;
    mem.begin(title, w, h);
    return DialogBoxIndirectParamW(pluginInstance(), mem.get(), parent, proc,
                                   reinterpret_cast<LPARAM>(&base));
}

// ---- single-line prompt dialog (rename, pairing code entry) ----

struct PromptCtx : DialogBase
{
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
        if (LOWORD(wp) == IDCANCEL) {
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

// Shows a one-line input. Returns false on cancel.
bool prompt(HWND parent, const std::wstring& title, const std::wstring& label, const std::string& initial,
            std::string& out) {
    PromptCtx c;
    c.label = label;
    c.initial = widen(initial);
    DialogMemory mem;
    mem.begin(title.c_str(), layout::kPrompt.pixelW, layout::kPrompt.pixelH);
    // Controls are created in the proc; template only provides the frame.
    INT_PTR r = DialogBoxIndirectParamW(
        pluginInstance(), mem.get(), parent,
        [](HWND dlg, UINT msg, WPARAM wp, LPARAM lp) -> INT_PTR {
            if (msg == WM_INITDIALOG) {
                // dialog-frame:kPrompt
                // Create the prompt controls lazily (keeps template trivial).
                makeLabel(dlg, L"", 16, 12, layout::kPrompt.pixelW - 32, 36, 100);
                makeEdit(dlg, 101, 16, 56, layout::kPrompt.pixelW - 32, 24);
                makeButton(dlg, IDOK, L"OK", 360, 96, layout::pushButtonPx(L"OK"), 28, BS_DEFPUSHBUTTON);
                makeButton(dlg, IDCANCEL, L"Cancel", 456, 96, layout::pushButtonPx(L"Cancel"), 28);
            }
            return promptProc(dlg, msg, wp, lp);
        },
        reinterpret_cast<LPARAM>(&c));
    if (r == IDOK && c.done) {
        out = c.result;
        return true;
    }
    return false;
}

} // namespace
// ============================ Sign In ============================

namespace
{
enum
{
    ID_SIGNIN_GOOGLE = 100,
    ID_SIGNIN_EMAIL,
    ID_SIGNIN_PASSWORD,
    ID_SIGNIN_CREATE,
    ID_SIGNIN_OK,
    ID_SIGNIN_CANCEL,
    ID_SIGNIN_STATUS
};

constexpr UINT_PTR kGooglePollTimer = 1;
constexpr ULONGLONG kGooglePollLimitMs = 10ull * 60ull * 1000ull;

struct SignInCtx : DialogBase
{
    std::string googleState;
    std::string googlePollSecret;
    bool googlePending = false;
    bool polling = false;
    ULONGLONG googleStartedAt = 0;
};

bool openHttpUrl(const std::string& url) {
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0)
        return false;
    HINSTANCE launched = ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(launched) > 32;
}

void setGoogleBusy(HWND dlg, bool busy) {
    EnableWindow(GetDlgItem(dlg, ID_SIGNIN_GOOGLE), busy ? FALSE : TRUE);
    EnableWindow(GetDlgItem(dlg, ID_SIGNIN_OK), busy ? FALSE : TRUE);
    EnableWindow(GetDlgItem(dlg, ID_SIGNIN_EMAIL), busy ? FALSE : TRUE);
    EnableWindow(GetDlgItem(dlg, ID_SIGNIN_PASSWORD), busy ? FALSE : TRUE);
}

INT_PTR CALLBACK signInProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        // dialog-frame:kSignIn
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        const int inner = layout::kSignIn.pixelW - 32;
        makeLabel(dlg, layout::kSignInLead, 16, 12, inner, 20);
        makeLabel(dlg, L"encryption keys stay on this device.", 16, 34, inner, 20);
        makeButton(dlg, ID_SIGNIN_GOOGLE, L"Sign in with Google", 16, 60, inner, 28, BS_DEFPUSHBUTTON);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 96, inner, 40, dlg,
                        (HMENU)(intptr_t)ID_SIGNIN_STATUS, nullptr, nullptr);
        makeLabel(dlg, L"Email and password (existing accounts)", 16, 144, inner, 20);
        makeLabel(dlg, L"Email:", 16, 172, 100, 20);
        makeEdit(dlg, ID_SIGNIN_EMAIL, 124, 170, layout::kSignIn.pixelW - 140, 22);
        makeLabel(dlg, L"Password:", 16, 200, 100, 20);
        makeEdit(dlg, ID_SIGNIN_PASSWORD, 124, 198, layout::kSignIn.pixelW - 140, 22, ES_PASSWORD);
        makeCheck(dlg, ID_SIGNIN_CREATE, layout::kCreateAccount, 124, 228,
                  layout::checkBoxPx(layout::kCreateAccount), false);
        makeButton(dlg, ID_SIGNIN_OK, L"Sign in with email", 124, 260,
                   layout::pushButtonPx(L"Sign in with email"), 28);
        makeButton(dlg, ID_SIGNIN_CANCEL, L"Cancel", 330, 260, layout::pushButtonPx(L"Cancel"), 28);
        setDefaultFont(dlg);
        return TRUE;
    }
    case WM_TIMER: {
        if (wp != kGooglePollTimer)
            break;
        auto& c = static_cast<SignInCtx&>(ctx(dlg));
        if (!c.googlePending || c.polling)
            return TRUE;
        if (GetTickCount64() - c.googleStartedAt > kGooglePollLimitMs) {
            KillTimer(dlg, kGooglePollTimer);
            c.googlePending = false;
            setGoogleBusy(dlg, false);
            setText(dlg, ID_SIGNIN_STATUS, L"Google sign-in timed out. Try again.");
            return TRUE;
        }
        c.polling = true;
        std::string err;
        auto st = c.engine->pollGoogleSignIn(c.googleState, c.googlePollSecret, err);
        c.polling = false;
        if (st == SyncEngine::GoogleSignInStatus::Pending) {
            if (!err.empty())
                setText(dlg, ID_SIGNIN_STATUS, widen(err));
            else
                setText(dlg, ID_SIGNIN_STATUS,
                        L"Waiting for Google... finish in the browser, then return here.");
            return TRUE;
        }
        KillTimer(dlg, kGooglePollTimer);
        c.googlePending = false;
        setGoogleBusy(dlg, false);
        if (st == SyncEngine::GoogleSignInStatus::Success) {
            c.done = true;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        setText(dlg, ID_SIGNIN_STATUS, widen(err));
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SIGNIN_GOOGLE: {
            auto& c = static_cast<SignInCtx&>(ctx(dlg));
            if (c.googlePending)
                return TRUE;
            std::string url, state, secret, err;
            if (!c.engine->startGoogleSignIn(url, state, secret, err)) {
                setText(dlg, ID_SIGNIN_STATUS, widen(err));
                return TRUE;
            }
            if (!openHttpUrl(url)) {
                setText(dlg, ID_SIGNIN_STATUS,
                        L"Could not open the browser. Copy the server URL and try again.");
                return TRUE;
            }
            c.googleState = state;
            c.googlePollSecret = secret;
            c.googlePending = true;
            c.googleStartedAt = GetTickCount64();
            setGoogleBusy(dlg, true);
            setText(dlg, ID_SIGNIN_STATUS, L"Waiting for Google... finish in the browser, then return here.");
            SetTimer(dlg, kGooglePollTimer, 1000, nullptr);
            return TRUE;
        }
        case ID_SIGNIN_OK: {
            auto& c = static_cast<SignInCtx&>(ctx(dlg));
            if (c.googlePending)
                return TRUE;
            std::string email = narrow(editText(GetDlgItem(dlg, ID_SIGNIN_EMAIL)));
            std::string password = narrow(editText(GetDlgItem(dlg, ID_SIGNIN_PASSWORD)));
            bool create = SendMessageW(GetDlgItem(dlg, ID_SIGNIN_CREATE), BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (email.empty() || password.empty()) {
                setText(dlg, ID_SIGNIN_STATUS, L"Email and password are required.");
                return TRUE;
            }
            std::string err;
            if (c.engine->signIn(email, password, err, create)) {
                c.done = true;
                EndDialog(dlg, IDOK);
            }
            else {
                setText(dlg, ID_SIGNIN_STATUS, widen(err));
            }
            return TRUE;
        }
        case ID_SIGNIN_CANCEL:
            KillTimer(dlg, kGooglePollTimer);
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        KillTimer(dlg, kGooglePollTimer);
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    case WM_DESTROY:
        KillTimer(dlg, kGooglePollTimer);
        break;
    }
    return FALSE;
}
} // namespace

bool Dialogs::showSignIn(HWND parent, SyncEngine& engine) {
    SignInCtx base;
    base.engine = &engine;
    INT_PTR r = runModal(parent, layout::kTitleSignIn, layout::kSignIn.pixelW, layout::kSignIn.pixelH,
                         signInProc, base);
    return r == IDOK && base.done;
}

// ============================ Status ============================

namespace
{
enum
{
    ID_ST_TEXT = 100,
    ID_ST_REFRESH,
    ID_ST_DEVICES
};

void fillStatus(HWND dlg, SyncEngine& e) {
    StatusInfo st = e.status();
    wchar_t buf[640];
    swprintf(buf, 640,
             L"Status: %hs\r\nLast sync: %hs\r\n\r\nFiles synchronized: %d\r\n"
             L"Pending uploads: %d\r\nPending downloads: %d\r\nConflicts: %d",
             st.statusText.c_str(), st.lastSyncTime.empty() ? "never" : st.lastSyncTime.c_str(),
             st.filesSynchronized, st.pendingUploads, st.pendingDownloads, st.conflicts);
    setText(dlg, ID_ST_TEXT, buf);

    std::vector<SyncEngine::DeviceInfo> devs;
    std::string err;
    std::wstring lines;
    if (e.listDevices(devs, err)) {
        for (auto& d : devs) {
            if (d.revoked)
                continue;
            lines += d.current ? L"- " : L"  ";
            lines += widen(d.name);
            lines += d.current ? L"  (this device)" : L"";
            lines += L"\r\n";
        }
    }
    if (lines.empty())
        lines = L"(device list unavailable offline)";
    setText(dlg, ID_ST_DEVICES, lines);
}

INT_PTR CALLBACK statusProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        // dialog-frame:kStatus
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        const int inner = layout::kStatus.pixelW - 32;
        makeLabel(dlg, L"Notepad++ Sync", 16, 12, 200, 20);
        makeEdit(dlg, ID_ST_TEXT, 16, 36, inner, 140, ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0);
        makeLabel(dlg, L"Devices:", 16, 184, 200, 20);
        makeEdit(dlg, ID_ST_DEVICES, 16, 208, inner, 100, ES_MULTILINE | ES_READONLY, 0);
        makeButton(dlg, ID_ST_REFRESH, L"Refresh", 233, 316, layout::pushButtonPx(L"Refresh"), 28);
        makeButton(dlg, IDOK, L"Close", 336, 316, layout::pushButtonPx(L"Close"), 28, BS_DEFPUSHBUTTON);
        setDefaultFont(dlg);
        fillStatus(dlg, *ctx(dlg).engine);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_ST_REFRESH) {
            fillStatus(dlg, *ctx(dlg).engine);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(dlg, IDOK);
        return TRUE;
    }
    return FALSE;
}
} // namespace

void Dialogs::showStatus(HWND parent, SyncEngine& engine) {
    DialogBase base{&engine, false, ""};
    runModal(parent, layout::kTitleStatus, layout::kStatus.pixelW, layout::kStatus.pixelH, statusProc, base);
}

// ============================ Second computer ============================

namespace
{
constexpr UINT_PTR kPairPollTimer = 2;
constexpr ULONGLONG kPairPollLimitMs = 5ull * 60ull * 1000ull + 15ull * 1000ull;

enum
{
    ID_PAIR_CODE = 100,
    ID_PAIR_STATUS = 101
};

struct PairWaitCtx : DialogBase
{
    std::string code;
    bool installed = false;
    bool polling = false;
    ULONGLONG startedAt = 0;
};

INT_PTR CALLBACK pairWaitProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        // dialog-frame:kPair
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        auto& c = static_cast<PairWaitCtx&>(ctx(dlg));
        const int inner = layout::kPair.pixelW - 32;
        makeLabel(dlg, layout::kPairLead, 16, 16, inner, 20);
        makeLabel(dlg, layout::kPairMenu, 16, 40, inner, 20);
        makeLabel(dlg, L"Type this code there. Leave this window open.", 16, 64, inner, 20);
        makeLabel(dlg, L"", 16, 96, inner, 32, ID_PAIR_CODE);
        setText(dlg, ID_PAIR_CODE, widen(c.code));
        makeLabel(dlg, L"Waiting for the other computer...", 16, 136, inner, 40, ID_PAIR_STATUS);
        makeButton(dlg, IDCANCEL, L"Cancel", 496, 188, layout::pushButtonPx(L"Cancel"), 28);
        setDefaultFont(dlg);
        c.startedAt = GetTickCount64();
        SetTimer(dlg, kPairPollTimer, 1500, nullptr);
        return TRUE;
    }
    case WM_TIMER: {
        if (wp != kPairPollTimer)
            break;
        auto& c = static_cast<PairWaitCtx&>(ctx(dlg));
        if (c.polling || c.installed)
            return TRUE;
        if (GetTickCount64() - c.startedAt > kPairPollLimitMs) {
            KillTimer(dlg, kPairPollTimer);
            setText(dlg, ID_PAIR_STATUS,
                    L"That code expired. Close this window and choose Get my notes again.");
            return TRUE;
        }
        c.polling = true;
        std::string err;
        auto st = c.engine->completePairing(c.code, err);
        c.polling = false;
        if (st == SyncEngine::PairingStatus::Pending) {
            if (!err.empty())
                setText(dlg, ID_PAIR_STATUS, widen(err));
            else
                setText(dlg, ID_PAIR_STATUS, L"Waiting for the other computer...");
            return TRUE;
        }
        KillTimer(dlg, kPairPollTimer);
        if (st == SyncEngine::PairingStatus::Installed) {
            c.installed = true;
            c.done = true;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        setText(dlg, ID_PAIR_STATUS, widen(err));
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) {
            KillTimer(dlg, kPairPollTimer);
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        KillTimer(dlg, kPairPollTimer);
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    case WM_DESTROY:
        KillTimer(dlg, kPairPollTimer);
        break;
    }
    return FALSE;
}

bool waitForOtherComputer(HWND parent, SyncEngine& engine, const std::string& code) {
    PairWaitCtx c;
    c.engine = &engine;
    c.code = code;
    INT_PTR r =
        runModal(parent, layout::kTitlePair, layout::kPair.pixelW, layout::kPair.pixelH, pairWaitProc, c);
    return r == IDOK && c.installed && engine.hasMasterKey();
}
} // namespace

void Dialogs::allowAnotherComputer(HWND parent, SyncEngine& engine) {
    if (!engine.isSignedIn()) {
        MessageBoxW(parent, L"Sign in with Google first.\n\nPlugins -> Notepad++ Sync -> Sign In.",
                    L"Notepad++ Sync", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!engine.hasMasterKey()) {
        MessageBoxW(parent,
                    L"This computer does not have your notes yet, so it cannot allow another one.\n\n"
                    L"On this computer choose Get my notes.\n"
                    L"On the computer that already has your notes choose Allow another computer, "
                    L"and type the code shown here.",
                    L"Notepad++ Sync", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::string code;
    if (!prompt(parent, L"Allow another computer", L"Type the code shown on the other computer (ABCD-EFGH):",
                "", code))
        return;
    std::string err;
    if (engine.approvePairing(code, err)) {
        MessageBoxW(parent,
                    L"Allowed. If the other computer's window is still open, it finishes by itself. "
                    L"You do not type the code again over there.",
                    L"Notepad++ Sync", MB_OK | MB_ICONINFORMATION);
    }
    else {
        MessageBoxW(parent, widen(err).c_str(), L"Notepad++ Sync", MB_OK | MB_ICONWARNING);
    }
}

bool Dialogs::getMyNotes(HWND parent, SyncEngine& engine) {
    if (!engine.isSignedIn()) {
        MessageBoxW(parent, L"Sign in with Google first.\n\nPlugins -> Notepad++ Sync -> Sign In.",
                    L"Notepad++ Sync", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    if (engine.hasMasterKey()) {
        int replace =
            MessageBoxW(parent,
                        L"This computer already has an encryption key.\n\n"
                        L"Continuing replaces it with the key from the computer that already has your notes. "
                        L"Notes encrypted only with the key on this computer will not open.\n\n"
                        L"Continue?",
                        L"Notepad++ Sync", MB_YESNO | MB_ICONWARNING);
        if (replace != IDYES)
            return engine.hasMasterKey();
    }
    std::string code, err;
    if (!engine.pairNewDevice(code, err)) {
        MessageBoxW(parent, widen(err.empty() ? std::string("Could not start. Try again.") : err).c_str(),
                    L"Notepad++ Sync", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (!waitForOtherComputer(parent, engine, code)) {
        if (!engine.hasMasterKey()) {
            MessageBoxW(
                parent,
                L"This computer still does not have the encryption key, so it did not create a new one.\n\n"
                L"Your notes stay unreadable until the other computer allows this one. "
                L"Choose Get my notes again when that computer is on.",
                L"Notepad++ Sync", MB_OK | MB_ICONINFORMATION);
        }
        else {
            MessageBoxW(parent,
                        L"The other computer has not allowed this one yet. "
                        L"The encryption key already on this computer was left as it is.",
                        L"Notepad++ Sync", MB_OK | MB_ICONINFORMATION);
        }
        return engine.hasMasterKey();
    }
    MessageBoxW(parent,
                L"This computer can now read your notes. They show up after the next sync, "
                L"in the folders you choose.",
                L"Notepad++ Sync", MB_OK | MB_ICONINFORMATION);
    return true;
}

// ============================ Devices ============================

namespace
{
enum
{
    ID_DEV_LIST = 100,
    ID_DEV_REFRESH,
    ID_DEV_RENAME,
    ID_DEV_REVOKE,
    ID_DEV_PAIR,
    ID_DEV_APPROVE,
    ID_DEV_MSG
};

struct DevicesCtx : DialogBase
{
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
    setText(dlg, ID_DEV_MSG,
            L"New computer: Get my notes. This one, if it has the notes: Allow another computer.");
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
        // dialog-frame:kDevices
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        DevicesCtx& c = static_cast<DevicesCtx&>(ctx(dlg));
        const int inner = layout::kDevices.pixelW - 32;
        HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_TABSTOP, 16, 12,
                                  inner, 188, dlg, (HMENU)(intptr_t)ID_DEV_LIST, nullptr, nullptr);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT);
        const wchar_t* cols[] = {L"Name", L"Device ID", L"Last seen", L"Status"};
        int widths[] = {160, 180, 120, 90};
        for (int i = 0; i < 4; ++i) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = const_cast<wchar_t*>(cols[i]);
            col.cx = widths[i];
            ListView_InsertColumn(lv, i, &col);
        }
        makeButton(dlg, ID_DEV_REFRESH, L"Refresh", 16, 208, layout::pushButtonPx(L"Refresh"), 28);
        makeButton(dlg, ID_DEV_RENAME, L"Rename", 119, 208, layout::pushButtonPx(L"Rename"), 28);
        makeButton(dlg, ID_DEV_REVOKE, L"Revoke", 215, 208, layout::pushButtonPx(L"Revoke"), 28);
        makeButton(dlg, ID_DEV_PAIR, L"Get my notes", 311, 208, layout::pushButtonPx(L"Get my notes"), 28);
        makeButton(dlg, ID_DEV_APPROVE, L"Allow another computer", 16, 244,
                   layout::pushButtonPx(L"Allow another computer"), 28);
        makeButton(dlg, IDOK, L"Close", 496, 244, layout::pushButtonPx(L"Close"), 28, BS_DEFPUSHBUTTON);
        makeLabel(dlg, L"New computer: Get my notes. This one, if it has the notes: Allow another computer.",
                  16, 284, inner, 40, ID_DEV_MSG);
        setDefaultFont(dlg);
        fillDevices(dlg, c);
        return TRUE;
    }
    case WM_COMMAND: {
        DevicesCtx& c = static_cast<DevicesCtx&>(ctx(dlg));
        int sel = ListView_GetNextItem(GetDlgItem(dlg, ID_DEV_LIST), -1, LVNI_SELECTED);
        switch (LOWORD(wp)) {
        case ID_DEV_REFRESH:
            fillDevices(dlg, c);
            return TRUE;
        case ID_DEV_RENAME: {
            if (sel < 0 || sel >= (int)c.devs.size())
                return TRUE;
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
            if (sel < 0 || sel >= (int)c.devs.size())
                return TRUE;
            if (c.devs[sel].current) {
                setText(dlg, ID_DEV_MSG, L"Use Sign Out to remove this device.");
                return TRUE;
            }
            std::wstring q =
                L"Revoke device \"" + widen(c.devs[sel].name) + L"\"? It will be signed out immediately.";
            if (MessageBoxW(dlg, q.c_str(), L"Notepad++ Sync", MB_YESNO | MB_ICONWARNING) == IDYES) {
                std::string err;
                if (!c.engine->revokeDeviceById(c.devs[sel].id, err))
                    setText(dlg, ID_DEV_MSG, widen(err));
                fillDevices(dlg, c);
            }
            return TRUE;
        }
        case ID_DEV_PAIR:
            Dialogs::getMyNotes(dlg, *c.engine);
            fillDevices(dlg, c);
            return TRUE;
        case ID_DEV_APPROVE:
            Dialogs::allowAnotherComputer(dlg, *c.engine);
            return TRUE;
        case IDOK:
        case IDCANCEL:
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        EndDialog(dlg, IDOK);
        return TRUE;
    }
    return FALSE;
}
} // namespace

void Dialogs::showDevices(HWND parent, SyncEngine& engine) {
    ensureCommonControls();
    DevicesCtx c;
    c.engine = &engine;
    DialogMemory mem;
    mem.begin(layout::kTitleDevices, layout::kDevices.pixelW, layout::kDevices.pixelH);
    DialogBoxIndirectParamW(pluginInstance(), mem.get(), parent, devicesProc, reinterpret_cast<LPARAM>(&c));
}
// ============================ Synced Files/Folders ============================

namespace
{
enum
{
    ID_SF_LIST = 100,
    ID_SF_ADDFOLDER,
    ID_SF_ADDFILE,
    ID_SF_REMOVE,
    ID_SF_IGNORE,
    ID_SF_SAVEIGNORE,
    ID_SF_MSG
};

struct SyncedFilesCtx : DialogBase
{
    std::vector<std::pair<std::string, std::wstring>> rows; // (id, path)
    std::vector<bool> isFolder;
};

void fillSyncedFiles(HWND dlg, SyncedFilesCtx& c) {
    HWND lv = GetDlgItem(dlg, ID_SF_LIST);
    ListView_DeleteAllItems(lv);
    c.rows.clear();
    c.isFolder.clear();
    for (auto& r : c.engine->db()->listSyncRoots()) {
        c.rows.push_back(r);
        c.isFolder.push_back(true);
    }
    for (auto& r : c.engine->db()->listSyncFiles()) {
        c.rows.push_back(r);
        c.isFolder.push_back(false);
    }
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
        // dialog-frame:kSyncedFiles
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        SyncedFilesCtx& c = static_cast<SyncedFilesCtx&>(ctx(dlg));
        const int inner = layout::kSyncedFiles.pixelW - 32;
        HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_TABSTOP, 16, 12,
                                  inner, 168, dlg, (HMENU)(intptr_t)ID_SF_LIST, nullptr, nullptr);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t*>(L"Type");
        col.cx = 80;
        ListView_InsertColumn(lv, 0, &col);
        col.pszText = const_cast<wchar_t*>(L"Path");
        col.cx = 500;
        ListView_InsertColumn(lv, 1, &col);

        makeButton(dlg, ID_SF_ADDFOLDER, layout::kAddFolder, 16, 188,
                   layout::pushButtonPx(layout::kAddFolder), 28);
        makeButton(dlg, ID_SF_ADDFILE, layout::kAddFile, 173, 188, layout::pushButtonPx(layout::kAddFile),
                   28);
        makeButton(dlg, ID_SF_REMOVE, layout::kRemoveSelected, 312, 188,
                   layout::pushButtonPx(layout::kRemoveSelected), 28);

        makeLabel(dlg,
                  L"Ignore patterns (one per line, .gitignore-style; a .npsyncignore file in a synced folder "
                  L"also applies):",
                  16, 224, inner, 40);
        makeEdit(dlg, ID_SF_IGNORE, 16, 272, inner, 100, ES_MULTILINE | WS_VSCROLL | ES_WANTRETURN,
                 WS_EX_CLIENTEDGE);
        makeButton(dlg, ID_SF_SAVEIGNORE, layout::kSavePatterns, 16, 384,
                   layout::pushButtonPx(layout::kSavePatterns), 28);
        makeButton(dlg, IDOK, L"Close", 536, 384, layout::pushButtonPx(L"Close"), 28, BS_DEFPUSHBUTTON);
        makeLabel(dlg, L"", 176, 388, 340, 20, ID_SF_MSG);
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
            if (sel < 0 || sel >= (int)c.rows.size())
                return TRUE;
            std::wstring q =
                L"Stop syncing\r\n" + c.rows[sel].second + L"\r\n\r\n(Local files are NOT deleted.)";
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
                if (ch == L'\r')
                    continue;
                if (ch == L'\n') {
                    if (!line.empty())
                        pats.push_back(narrow(line));
                    line.clear();
                }
                else
                    line.push_back(ch);
            }
            if (!line.empty())
                pats.push_back(narrow(line));
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
    case WM_CLOSE:
        EndDialog(dlg, IDOK);
        return TRUE;
    }
    return FALSE;
}
} // namespace

void Dialogs::showSyncedFiles(HWND parent, SyncEngine& engine) {
    ensureCommonControls();
    SyncedFilesCtx c;
    c.engine = &engine;
    DialogMemory mem;
    mem.begin(layout::kTitleSyncedFiles, layout::kSyncedFiles.pixelW, layout::kSyncedFiles.pixelH);
    DialogBoxIndirectParamW(pluginInstance(), mem.get(), parent, syncedFilesProc,
                            reinterpret_cast<LPARAM>(&c));
}

// ============================ Conflicts ============================

namespace
{
enum
{
    ID_CF_LIST = 100,
    ID_CF_LOCAL,
    ID_CF_REMOTE,
    ID_CF_BOTH,
    ID_CF_COMPARE,
    ID_CF_REFRESH,
    ID_CF_MSG
};

struct ConflictsCtx : DialogBase
{
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
        // dialog-frame:kConflicts
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        ConflictsCtx& c = static_cast<ConflictsCtx&>(ctx(dlg));
        const int inner = layout::kConflicts.pixelW - 32;
        HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_TABSTOP, 16, 12,
                                  inner, 160, dlg, (HMENU)(intptr_t)ID_CF_LIST, nullptr, nullptr);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT);
        const wchar_t* cols[] = {L"File", L"Version", L"Detected"};
        int widths[] = {400, 110, 100};
        for (int i = 0; i < 3; ++i) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = const_cast<wchar_t*>(cols[i]);
            col.cx = widths[i];
            ListView_InsertColumn(lv, i, &col);
        }
        makeLabel(dlg, L"Both versions are always preserved. Choose a resolution:", 16, 180, inner, 20);
        makeButton(dlg, ID_CF_LOCAL, L"Keep Local", 16, 208, layout::pushButtonPx(L"Keep Local"), 28);
        makeButton(dlg, ID_CF_REMOTE, L"Keep Remote", 146, 208, layout::pushButtonPx(L"Keep Remote"), 28);
        makeButton(dlg, ID_CF_BOTH, L"Keep Both", 285, 208, layout::pushButtonPx(L"Keep Both"), 28);
        makeButton(dlg, ID_CF_COMPARE, L"Open Comparison", 406, 208, layout::pushButtonPx(L"Open Comparison"),
                   28);
        makeButton(dlg, ID_CF_REFRESH, L"Refresh", 16, 252, layout::pushButtonPx(L"Refresh"), 28);
        makeButton(dlg, IDOK, L"Close", 556, 252, layout::pushButtonPx(L"Close"), 28, BS_DEFPUSHBUTTON);
        makeLabel(dlg, L"", 120, 256, 420, 20, ID_CF_MSG);
        setDefaultFont(dlg);
        fillConflicts(dlg, c);
        return TRUE;
    }
    case WM_COMMAND: {
        ConflictsCtx& c = static_cast<ConflictsCtx&>(ctx(dlg));
        int sel = ListView_GetNextItem(GetDlgItem(dlg, ID_CF_LIST), -1, LVNI_SELECTED);
        auto resolve = [&](const char* strategy) {
            if (sel < 0 || sel >= (int)c.items.size())
                return;
            std::string err;
            if (c.engine->resolveConflict(c.items[sel].fileId, strategy, err))
                setText(dlg, ID_CF_MSG, L"Resolved.");
            else
                setText(dlg, ID_CF_MSG, widen(err));
            fillConflicts(dlg, c);
        };
        switch (LOWORD(wp)) {
        case ID_CF_LOCAL:
            resolve("keepLocal");
            return TRUE;
        case ID_CF_REMOTE:
            resolve("keepRemote");
            return TRUE;
        case ID_CF_BOTH:
            resolve("keepBoth");
            return TRUE;
        case ID_CF_COMPARE: {
            if (sel < 0 || sel >= (int)c.items.size())
                return TRUE;
            // Open the preserved local copy and the canonical file side by side.
            std::wstring localCopy = widen(c.items[sel].localCopyPath);
            HWND npp = nppHandle();
            if (npp && !localCopy.empty())
                ::SendMessageW(npp, NPPM_DOOPEN, 0, (LPARAM)localCopy.c_str());
            return TRUE;
        }
        case ID_CF_REFRESH:
            fillConflicts(dlg, c);
            return TRUE;
        case IDOK:
        case IDCANCEL:
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        EndDialog(dlg, IDOK);
        return TRUE;
    }
    return FALSE;
}
} // namespace

void Dialogs::showConflicts(HWND parent, SyncEngine& engine) {
    ensureCommonControls();
    ConflictsCtx c;
    c.engine = &engine;
    DialogMemory mem;
    mem.begin(layout::kTitleConflicts, layout::kConflicts.pixelW, layout::kConflicts.pixelH);
    DialogBoxIndirectParamW(pluginInstance(), mem.get(), parent, conflictsProc, reinterpret_cast<LPARAM>(&c));
}
// ============================ Settings (tabbed) ============================

namespace
{
enum
{
    ID_SET_TAB = 100,
    ID_SET_OK,
    ID_SET_CANCEL,
    ID_SET_MSG,
    // General
    ID_G_AUTOSTART = 200,
    ID_G_PAUSE,
    ID_G_INTERVAL,
    ID_G_WS,
    ID_G_NOTIFY,
    // Files
    ID_F_MAXSIZE,
    ID_F_IGNORE,
    // Session
    ID_S_FILESONLY,
    ID_S_TABS,
    ID_S_CURSOR,
    ID_S_UNSAVED,
    // Security
    ID_SEC_DEVNAME,
    ID_SEC_RECOVERY,
    ID_SEC_DEVICES,
    // Advanced
    ID_A_URL,
    ID_A_DEBUG,
    ID_A_DBLOC,
    ID_A_RESET,
};

struct SettingsCtx : DialogBase
{
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
        // dialog-frame:kSettings
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        SettingsCtx& c = static_cast<SettingsCtx&>(ctx(dlg));
        Settings* s = c.engine ? c.engine->settings() : nullptr;
        const int inner = layout::kSettings.pixelW - 32;
        HWND tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 8, 8,
                                   layout::kSettings.pixelW - 16, 28, dlg, (HMENU)(intptr_t)ID_SET_TAB,
                                   nullptr, nullptr);
        const wchar_t* tabs[] = {L"General", L"Files", L"Session", L"Security", L"Advanced"};
        for (int i = 0; i < 5; ++i) {
            TCITEMW ti{};
            ti.mask = TCIF_TEXT;
            ti.pszText = const_cast<wchar_t*>(tabs[i]);
            TabCtrl_InsertItem(tab, i, &ti);
        }
        const int tx = 16, ty = 48; // tab content origin

        // General
        makeCheck(dlg, ID_G_AUTOSTART, L"Start sync automatically", tx, ty,
                  layout::checkBoxPx(L"Start sync automatically"), s->startSyncAutomatically);
        makeCheck(dlg, ID_G_PAUSE, L"Pause sync", tx, ty + 28, layout::checkBoxPx(L"Pause sync"),
                  s->pauseSync);
        makeCheck(dlg, ID_G_WS, L"Realtime connection (WebSocket)", tx, ty + 56,
                  layout::checkBoxPx(L"Realtime connection (WebSocket)"), s->webSocketEnabled);
        makeCheck(dlg, ID_G_NOTIFY, L"Notifications", tx, ty + 84, layout::checkBoxPx(L"Notifications"),
                  s->notificationsEnabled);
        makeLabel(dlg, L"Sync interval fallback (sec):", tx, ty + 120,
                  layout::labelPx(L"Sync interval fallback (sec):"), 20, 219);
        makeEdit(dlg, ID_G_INTERVAL, 308, ty + 118, 70, 22);
        setText(dlg, ID_G_INTERVAL, std::to_string(s->syncIntervalFallbackSec));

        // Files
        makeLabel(dlg, L"Max file size (MB):", tx, ty, layout::labelPx(L"Max file size (MB):"), 20, 238);
        makeEdit(dlg, ID_F_MAXSIZE, 220, ty - 2, 70, 22);
        setText(dlg, ID_F_MAXSIZE, std::to_string(s->maxFileBytes / 1024 / 1024));
        makeLabel(dlg, L"Manage synced folders/files with 'Synced Files/Folders' in the plugin menu.", tx,
                  ty + 32, inner, 40, 239);
        makeLabel(dlg, L"Ignore patterns:", tx, ty + 80, layout::labelPx(L"Ignore patterns:"), 20, 237);
        makeEdit(dlg, ID_F_IGNORE, tx, ty + 104, inner, 100, ES_MULTILINE | WS_VSCROLL | ES_WANTRETURN,
                 WS_EX_CLIENTEDGE);
        {
            std::wstring pats;
            for (auto& p : s->extraIgnorePatterns) {
                pats += widen(p);
                pats += L"\r\n";
            }
            setText(dlg, ID_F_IGNORE, pats);
        }

        // Session
        makeButton(dlg, ID_S_FILESONLY, L"Sync files only", tx, ty, 420, 22, BS_AUTORADIOBUTTON | WS_GROUP);
        makeButton(dlg, ID_S_TABS, L"Sync files + open tabs", tx, ty + 30, 420, 22, BS_AUTORADIOBUTTON);
        makeButton(dlg, ID_S_CURSOR, L"Sync files + tabs + cursor positions", tx, ty + 60, 420, 22,
                   BS_AUTORADIOBUTTON);
        CheckRadioButton(dlg, ID_S_FILESONLY, ID_S_CURSOR, ID_S_FILESONLY + (int)s->sessionMode);
        makeCheck(dlg, ID_S_UNSAVED, L"Sync unsaved notes (WARNING: uploads never-saved scratch content)", tx,
                  ty + 96,
                  layout::checkBoxPx(L"Sync unsaved notes (WARNING: uploads never-saved scratch content)"),
                  s->syncUnsavedNotes);

        // Security
        makeLabel(dlg, L"This device name:", tx, ty, layout::labelPx(L"This device name:"), 20, 279);
        makeEdit(dlg, ID_SEC_DEVNAME, 210, ty - 2, 250, 22);
        setText(dlg, ID_SEC_DEVNAME, s->deviceName);
        makeButton(dlg, ID_SEC_RECOVERY, L"Show recovery key", tx, ty + 36,
                   layout::pushButtonPx(L"Show recovery key"), 28);
        makeButton(dlg, ID_SEC_DEVICES, L"Manage devices...", tx, ty + 76,
                   layout::pushButtonPx(L"Manage devices..."), 28);

        // Advanced
        makeLabel(dlg, L"Backend URL:", tx, ty, layout::labelPx(L"Backend URL:"), 20, 298);
        makeEdit(dlg, ID_A_URL, tx, ty + 24, inner, 22);
        setText(dlg, ID_A_URL, s->backendUrl);
        makeCheck(dlg, ID_A_DEBUG, L"Debug logging", tx, ty + 60, layout::checkBoxPx(L"Debug logging"),
                  s->debugLogging);
        makeLabel(dlg, L"Database location (blank = default):", tx, ty + 92,
                  layout::labelPx(L"Database location (blank = default):"), 20, 297);
        makeEdit(dlg, ID_A_DBLOC, tx, ty + 116, inner, 22);
        setText(dlg, ID_A_DBLOC, s->databaseLocation);
        makeButton(dlg, ID_A_RESET, L"Reset local sync state...", tx, ty + 152,
                   layout::pushButtonPx(L"Reset local sync state..."), 28);

        makeButton(dlg, ID_SET_OK, L"Save", 480, 296, layout::pushButtonPx(L"Save"), 28, BS_DEFPUSHBUTTON);
        makeButton(dlg, ID_SET_CANCEL, L"Cancel", 576, 296, layout::pushButtonPx(L"Cancel"), 28);
        makeLabel(dlg, L"", 16, 300, 450, 20, ID_SET_MSG);

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
            std::wstring m =
                rk.empty()
                    ? L"No recovery key is stored on this device.\n\n"
                      L"A recovery key on another computer does not unlock this one. "
                      L"This computer gets the encryption key when the other computer "
                      L"chooses Allow another computer."
                    : L"Your recovery key (keep it with this computer):\n\n" + widen(rk) +
                          L"\n\nThis key only unwraps the copy stored for this Windows user. "
                          L"Typing it on another computer does not open your notes. "
                          L"The other computer gets the key when you choose Allow another computer here.";
            MessageBoxW(dlg, m.c_str(), layout::kTitleRecovery,
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
            try {
                s->syncIntervalFallbackSec =
                    std::max(5, std::stoi(narrow(editText(GetDlgItem(dlg, ID_G_INTERVAL)))));
            }
            catch (...) {
            }
            try {
                s->maxFileBytes =
                    (int64_t)std::max(1, std::stoi(narrow(editText(GetDlgItem(dlg, ID_F_MAXSIZE))))) * 1024 *
                    1024;
            }
            catch (...) {
            }
            {
                std::wstring raw = editText(GetDlgItem(dlg, ID_F_IGNORE));
                std::vector<std::string> pats;
                std::wstring line;
                for (wchar_t ch : raw) {
                    if (ch == L'\r')
                        continue;
                    if (ch == L'\n') {
                        if (!line.empty())
                            pats.push_back(narrow(line));
                        line.clear();
                    }
                    else
                        line.push_back(ch);
                }
                if (!line.empty())
                    pats.push_back(narrow(line));
                s->extraIgnorePatterns = pats;
            }
            if (checkState(dlg, ID_S_FILESONLY))
                s->sessionMode = SessionSyncMode::FilesOnly;
            else if (checkState(dlg, ID_S_TABS))
                s->sessionMode = SessionSyncMode::FilesAndTabs;
            else
                s->sessionMode = SessionSyncMode::FilesTabsCursor;
            s->syncUnsavedNotes = checkState(dlg, ID_S_UNSAVED);
            s->deviceName = narrow(editText(GetDlgItem(dlg, ID_SEC_DEVNAME)));
            std::string url = narrow(editText(GetDlgItem(dlg, ID_A_URL)));
            if (!url.empty())
                s->backendUrl = url;
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
    mem.begin(layout::kTitleSettings, layout::kSettings.pixelW, layout::kSettings.pixelH);
    DialogBoxIndirectParamW(pluginInstance(), mem.get(), parent, settingsProc, reinterpret_cast<LPARAM>(&c));
}

// ============================ First run & About ============================

void Dialogs::showFirstRunWizard(HWND parent, SyncEngine& engine) {
    int r = MessageBoxW(parent,
                        L"Welcome to Notepad++ Sync.\n\n"
                        L"Sign in with Google. If this is your first computer, an encryption key "
                        L"is created here and never uploaded. If your notes are already on another "
                        L"computer, that computer allows this one - this setup will not create a "
                        L"second key.\n\n"
                        L"You do not type a server address.\n\n"
                        L"Continue?",
                        layout::kTitleSetup, MB_YESNO | MB_ICONQUESTION);
    if (r != IDYES)
        return;

    if (!showSignIn(parent, engine))
        return;

    bool createHere = false;
    if (!engine.hasMasterKey()) {
        int which = MessageBoxW(parent,
                                L"Are your notes already on another computer?\n\n"
                                L"Yes - I will open Notepad++ there and choose Allow another computer.\n"
                                L"No - this is the first computer. Create the encryption key here.",
                                layout::kTitleSetup, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (which == IDCANCEL) {
            MessageBoxW(parent,
                        L"Setup stopped. This computer did not create an encryption key.\n\n"
                        L"When you are ready, choose Plugins -> Notepad++ Sync -> Get my notes.",
                        L"Notepad++ Sync", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (which == IDYES) {
            // Polls until the other computer wraps its master key. Does not mint.
            Dialogs::getMyNotes(parent, engine);
        }
        else {
            createHere = true;
        }
    }
    // finishFirstRunKeys keeps a key that pairing just installed, even when
    // createHere is true. It mints only when this is the first computer and
    // no key is present.
    auto step = engine.finishFirstRunKeys(createHere);
    if (step == SyncEngine::FirstRunKeyStep::CreatedNew) {
        std::string rk = engine.exportRecoveryKeyWrapped();
        std::wstring msg =
            rk.empty()
                ? L"An encryption key was created on this computer, but the recovery key could not be saved."
                : L"Your recovery key (keep it with this computer):\n\n" + widen(rk) +
                      L"\n\nThis key only unwraps the copy stored for this Windows user. "
                      L"Typing it on another computer does not open your notes. "
                      L"On the other computer, sign in with the same Google account and choose Yes "
                      L"when asked if your notes are already on another computer. Then, on this "
                      L"computer, choose Allow another computer and type the code it shows.\n\n"
                      L"If this computer is lost before another one is allowed, the notes on the "
                      L"server cannot be read. Google sign-in does not replace this key.";
        MessageBoxW(parent, msg.c_str(), layout::kTitleRecovery, MB_OK | MB_ICONWARNING);
    }
    // Setup ends here, after sign-in, the first-computer question, and the
    // recovery key. The folder list stays on the plugin menu for someone
    // who later wants to add a folder. It is not opened during setup.
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
