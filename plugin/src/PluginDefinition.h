// PluginDefinition.h — Notepad++ plugin registration: name, menu items,
// command dispatch, and notification routing.
#pragma once

#include "PluginInterface.h"

#include <string>

namespace npsync
{

constexpr int kMenuCount = 10;
extern const wchar_t kPluginName[];

// Menu command indices.
enum MenuCmd
{
    CmdSignIn = 0,
    CmdSignOut,
    CmdSyncNow,
    CmdStatus,
    CmdDevices,
    CmdSyncedFiles,
    CmdConflicts,
    CmdSettings,
    CmdPauseSync,
    CmdAbout,
};

// Called from DllMain.cpp (the extern "C" glue).
void pluginSetInfo(NppData data);
void pluginInit(HINSTANCE hModule);
void pluginCleanup();
FuncItem* pluginGetFuncsArray(int* count);
void pluginBeNotified(SCNotification* notify);
LRESULT pluginMessageProc(UINT msg, WPARAM wp, LPARAM lp);

// Access to the global engine (used by dialogs).
class SyncEngine;
SyncEngine& engine();
HWND nppHandle();

} // namespace npsync
