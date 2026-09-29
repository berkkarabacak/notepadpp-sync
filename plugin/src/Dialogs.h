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

// Computer that already has the notes: type the code shown on the new one.
// Wraps this device's master key. Does not poll.
void allowAnotherComputer(HWND parent, SyncEngine& engine);

// Computer that needs the notes: request a code, show it, and poll until
// the other computer allows it. On success the wrapped master key is installed.
// Returns true only when this device holds a master key afterwards.
bool getMyNotes(HWND parent, SyncEngine& engine);

// Settings (general/files/session/security/advanced tabs).
void showSettings(HWND parent, SyncEngine& engine);

// First-run setup wizard. Signs in, then either keeps a key delivered by
// pairing or mints one on the first computer. Does not mint when pairing
// already installed a key.
void showFirstRunWizard(HWND parent, SyncEngine& engine);

void showAbout(HWND parent);

} // namespace Dialogs

} // namespace npsync
