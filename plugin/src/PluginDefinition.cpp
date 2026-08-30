// PluginDefinition.cpp — menu setup, command handlers, and notification
// routing between Notepad++ and the sync engine.
#include "PluginDefinition.h"

#include "Dialogs.h"
#include "Logger.h"
#include "Notepad_plus_msgs.h"
#include "Settings.h"
#include "SyncEngine.h"
#include "core/PathUtil.h"

#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <memory>

namespace npsync {

const wchar_t kPluginName[] = L"Notepad++ Sync";

namespace {

NppData g_nppData;
HINSTANCE g_hModule = nullptr;
FuncItem g_funcItems[kMenuCount];

std::unique_ptr<SettingsStore> g_store;
std::unique_ptr<Settings> g_settings;
std::unique_ptr<SyncEngine> g_engine;
bool g_started = false;

std::wstring appDataDir() {
    wchar_t* roaming = nullptr;
    std::wstring base = L"%APPDATA%";
    size_t len = 0;
    if (_wdupenv_s(&roaming, &len, L"APPDATA") == 0 && roaming) {
        base = roaming;
        free(roaming);
    }
    return base + L"\\Notepad++Sync";
}

// Notepad++ API helpers ------------------------------------------------

std::wstring currentFilePath() {
    if (!g_nppData._nppHandle) return {};
    HWND npp = g_nppData._nppHandle;
    auto bufId = (int64_t)::SendMessageW(npp, NPPM_GETCURRENTBUFFERID, 0, 0);
    if (bufId == 0) return {};
    wchar_t path[MAX_PATH] = {0};
    ::SendMessageW(npp, NPPM_GETFULLPATHFROMBUFFERID, (WPARAM)bufId, (LPARAM)path);
    return path;
}

// Menu command handlers ------------------------------------------------

void cmdSignIn() {
    if (g_engine->isSignedIn()) {
        Dialogs::showStatus(g_nppData._nppHandle, *g_engine);
        return;
    }
    Dialogs::showSignIn(g_nppData._nppHandle, *g_engine);
}

void cmdSignOut() {
    if (MessageBoxW(g_nppData._nppHandle, L"Sign out of Notepad++ Sync on this device?",
                    kPluginName, MB_YESNO | MB_ICONQUESTION) == IDYES)
        g_engine->signOut();
}

void cmdSyncNow()       { g_engine->syncNow(); }
void cmdStatus()        { Dialogs::showStatus(g_nppData._nppHandle, *g_engine); }
void cmdDevices()       { Dialogs::showDevices(g_nppData._nppHandle, *g_engine); }
void cmdSyncedFiles()   { Dialogs::showSyncedFiles(g_nppData._nppHandle, *g_engine); }
void cmdConflicts()     { Dialogs::showConflicts(g_nppData._nppHandle, *g_engine); }
void cmdSettings()      { Dialogs::showSettings(g_nppData._nppHandle, *g_engine); }
void cmdAbout()         { Dialogs::showAbout(g_nppData._nppHandle); }

void cmdPauseSync() {
    bool paused = !g_settings->pauseSync;
    g_engine->setPaused(paused);
    // Reflect the toggle in the menu checkmark.
    ::SendMessageW(g_nppData._nppHandle, NPPM_SETMENUITEMCHECK,
                   (WPARAM)g_funcItems[CmdPauseSync]._cmdID, (LPARAM)paused);
}

void setMenuItem(int idx, const wchar_t* name, PFUNCPLUGINCMD fn) {
    wcscpy_s(g_funcItems[idx]._itemName, nbChar, name);
    g_funcItems[idx]._pFunc = fn;
    g_funcItems[idx]._cmdID = idx;
    g_funcItems[idx]._init2Check = false;
    g_funcItems[idx]._pShKey = nullptr;
}

void initMenu() {
    setMenuItem(CmdSignIn,      L"Sign In",                cmdSignIn);
    setMenuItem(CmdSignOut,     L"Sign Out",               cmdSignOut);
    setMenuItem(CmdSyncNow,     L"Sync Now",               cmdSyncNow);
    setMenuItem(CmdStatus,      L"Sync Status",            cmdStatus);
    setMenuItem(CmdDevices,     L"Manage Devices",         cmdDevices);
    setMenuItem(CmdSyncedFiles, L"Synced Files/Folders",   cmdSyncedFiles);
    setMenuItem(CmdConflicts,   L"Conflicts",              cmdConflicts);
    setMenuItem(CmdSettings,    L"Settings",               cmdSettings);
    setMenuItem(CmdPauseSync,   L"Pause Sync",             cmdPauseSync);
    setMenuItem(CmdAbout,       L"About",                  cmdAbout);
}

void startEngine() {
    if (g_started) return;
    g_started = true;

    std::wstring dir = appDataDir();
    Logger::init(dir + L"\\logs", g_settings->debugLogging ? LogLevel::Debug : LogLevel::Info);
    Logger::info("plugin starting");

    g_engine->init(g_store.get(), g_settings.get());
    g_engine->onStatusChanged = [] {
        // Marshalled on demand; status UI reads SyncEngine::status().
    };
    g_engine->onRemoteFileApplied = [](const std::wstring& absPath) {
        // If the file is open in Notepad++, reload its buffer (guarded:
        // only when it has no unsaved local edits would be ideal; Notepad++
        // shows its own "file changed" prompt otherwise).
        HWND npp = g_nppData._nppHandle;
        if (!npp) return;
        std::wstring open = currentFilePath();
        std::wstring a = absPath, b = open;
        for (auto& c : a) c = (wchar_t)towlower(c);
        for (auto& c : b) c = (wchar_t)towlower(c);
        if (a == b && !b.empty())
            ::SendMessageW(npp, NPPM_RELOADFILE, 0, (LPARAM)open.c_str());
    };
    g_engine->start();

    // First-run: no account configured yet.
    if (!g_engine->isSignedIn())
        Dialogs::showFirstRunWizard(g_nppData._nppHandle, *g_engine);
}

} // namespace

SyncEngine& engine() { return *g_engine; }
HWND nppHandle() { return g_nppData._nppHandle; }

void pluginInit(HINSTANCE hModule) {
    g_hModule = hModule;
    g_settings = std::make_unique<Settings>();
    g_store = std::make_unique<SettingsStore>(appDataDir());
    g_store->load(*g_settings);
    g_engine = std::make_unique<SyncEngine>();
    initMenu();
}

void pluginSetInfo(NppData data) { g_nppData = data; }

void pluginCleanup() {
    Logger::info("plugin shutting down");
    if (g_engine) g_engine->stop();
    g_engine.reset();
    g_store.reset();
    g_settings.reset();
}

FuncItem* pluginGetFuncsArray(int* count) {
    *count = kMenuCount;
    return g_funcItems;
}

void pluginBeNotified(SCNotification* notify) {
    if (!notify) return;
    switch (notify->nmhdr.code) {
    case NPPN_READY:
        startEngine();
        break;
    case NPPN_SHUTDOWN:
        pluginCleanup();
        break;
    case NPPN_FILESAVED: {
        wchar_t path[MAX_PATH] = {0};
        ::SendMessageW(g_nppData._nppHandle, NPPM_GETFULLPATHFROMBUFFERID,
                       (WPARAM)notify->nmhdr.idFrom, (LPARAM)path);
        if (path[0] && g_engine) g_engine->onFileSaved(path);
        break;
    }
    case NPPN_FILEOPENED:
    case NPPN_BUFFERACTIVATED: {
        wchar_t path[MAX_PATH] = {0};
        ::SendMessageW(g_nppData._nppHandle, NPPM_GETFULLPATHFROMBUFFERID,
                       (WPARAM)notify->nmhdr.idFrom, (LPARAM)path);
        if (path[0] && g_engine) g_engine->onFileOpened(path);
        break;
    }
    default:
        break;
    }
}

LRESULT pluginMessageProc(UINT, WPARAM, LPARAM) { return TRUE; }

} // namespace npsync
