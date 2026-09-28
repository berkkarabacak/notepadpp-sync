// Dialogs.h — small native Win32 dialogs for the plugin UI. Deliberately
// plain: the plugin should feel like Notepad++, not a separate app.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace npsync
{

class SyncEngine;

// Each dialog is modal against the Notepad++ main window.
namespace Dialogs
{

// Sign in. Google is the primary button; email/password stays for accounts
// that already have a password and for servers without Google configured.
bool showSignIn(HWND parent, SyncEngine& engine);

// Sync Status window (status, last sync, counts, devices).
void showStatus(HWND parent, SyncEngine& engine);

// Manage sync roots (folders/files) and ignore patterns.
void showSyncedFiles(HWND parent, SyncEngine& engine);

// Conflict list + resolution choices.
void showConflicts(HWND parent, SyncEngine& engine);

// Device management (list, rename, revoke, pair).
void showDevices(HWND parent, SyncEngine& engine);

// Settings (general/files/session/security/advanced tabs).
void showSettings(HWND parent, SyncEngine& engine);

// First-run setup wizard (account -> keys -> device name -> roots -> go).
void showFirstRunWizard(HWND parent, SyncEngine& engine);

void showAbout(HWND parent);

} // namespace Dialogs

} // namespace npsync
