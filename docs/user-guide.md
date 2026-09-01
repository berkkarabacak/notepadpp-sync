# User Guide — Notepad++ Sync

This guide is for people who just want their notes on all their laptops.
No command line required.

## 1. Install

1. Download the latest release ZIP (`NotepadPlusPlusSync-vX.Y.Z-win64.zip`)
   from the [Releases page](../../releases).
2. Close Notepad++.
3. Create the folder `%APPDATA%\Notepad++\plugins\NppSync\` (paste that path
   into Explorer's address bar) and copy `NppSync.dll` from the ZIP into it.
   - Alternatively, if you have admin rights, use
     `C:\Program Files\Notepad++\plugins\NppSync\` (system-wide install).
4. Open Notepad++. You should see **Plugins → Notepad++ Sync**.

> Windows may mark downloaded DLLs as blocked. If the plugin does not appear,
> right-click `NppSync.dll` → Properties → check **Unblock** → OK.

## 2. First-run setup

The first time the plugin loads, a short setup runs:

1. **Create an account or sign in.** Your password is only used to log in —
   it is never used to encrypt files and never leaves the plugin unhashed.
2. **Encryption keys are generated on your device.** You'll see a recovery
   key like `NPSYNC-XXXX-XXXX-XXXX-XXXX-XXXX`. **Write it down and keep it
   offline.** If you lose every device *and* this key, your notes are
   unrecoverable — by design, nobody (including the server) can reset it.
3. **Name this device** (e.g. `Laptop-Home`) so you recognize it later.
4. **Choose files/folders to sync.**
5. Done. Sync runs quietly in the background from now on.

## 3. Everyday use

Just use Notepad++. Save a file on one laptop; it appears on the other
within a few seconds. That's it.

The plugin menu offers:

- **Sync Now** — force a sync cycle (rarely needed).
- **Sync Status** — status, last sync time, pending items, conflicts.
- **Pause Sync** — suspend syncing until you resume.

The status indicator shows one of: `Synced`, `Syncing`, `Offline`,
`Conflict`, `Error`.

### Offline

Work normally without internet. Changes queue locally and sync when you're
back online — even across Notepad++ restarts or reboots.

## 4. Conflicts

If two laptops edit the same file while offline (or at the same moment),
both versions are kept — always. Text files are merged automatically when
the edits don't overlap. When they do, **Conflicts** in the menu offers:

- **Keep Local** — this laptop's version wins (uploaded as a new version).
- **Keep Remote** — the other device's version wins.
- **Keep Both** — remote version applies; your version is saved next to the
  file as `name (conflict - Laptop-B - 2026-08-30).txt`.

Nothing is ever silently discarded.

## 5. Adding a second laptop

1. Install the plugin on the new laptop and sign in.
2. On the new device: **Manage Devices → pair** — a code like `ABCD-EFGH`
   appears.
3. On any existing device: approve the pairing and enter the code. The
   encryption key is transferred securely (the server can't read it).
4. Alternatively, use your offline recovery key.

## 6. Choosing what to sync

**Synced Files/Folders** lets you add folders or individual files. To
exclude things, create a `.npsyncignore` file inside a synced folder:

```
*.tmp
*.log
.git/
node_modules/
backup/
```

(Same syntax as `.gitignore`.)

## 7. Version history

Every synced version (30 by default) is kept. Open **Sync Status** to see
counts, and restore older versions from there.

## 8. Session sync (optional)

In **Settings → Session** you can additionally sync open tabs, the selected
tab, and cursor/scroll positions. Unsaved `new 1`-style tabs are **never**
uploaded unless you explicitly enable the clearly-labeled *Sync Unsaved
Notes* option.

## 9. Uninstall

Delete `NppSync.dll` from the plugins folder. Local data (settings, queue,
logs) lives in `%APPDATA%\Notepad++Sync\` and can be deleted too. Your
encrypted files remain on the server until you delete your account.
