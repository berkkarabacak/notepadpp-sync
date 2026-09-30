# User Guide — Notepad++ Sync

This guide is for people who just want their notes on all their laptops.
No command line required.

## 1. Install

Use the setup program. You do not copy a DLL by hand.

1. Download `NotepadPlusPlusSync-vX.Y.Z-win64-setup.exe` from the
   [Releases page](../../releases).
2. Double-click it.
3. If Windows says it protected your PC, choose **More info**, then
   **Run anyway**. The download is not signed yet.
4. If Windows asks "Do you want to allow this app to make changes?",
   choose **Yes**. That only lets the installer copy the plugin into the
   Notepad++ folder.
5. Choose **Next**, then **Install**.
6. If Notepad++ is open, the installer asks before it closes it. Choose
   **Yes** only when you are ready. Notepad++ then asks you to save any
   unsaved notes. The installer does not force Notepad++ to quit. Choose
   **No** to stop, close Notepad++ yourself, and run the installer again.
7. Open Notepad++. You should see **Plugins → Notepad++ Sync**.

You are not asked for a server address. A new install connects to
`https://sync.berkkarabacak.com`.

The installer looks for 64-bit Notepad++. A normal install is
`C:\Program Files\Notepad++`. If Notepad++ was installed for your Windows
user only, it looks in that install (often
`%LOCALAPPDATA%\Programs\Notepad++`). It copies `NppSync.dll`, and `deps\`
when the release includes that folder, into `plugins\NppSync` there.

### If you cannot run the setup program

1. Download `NotepadPlusPlusSync-vX.Y.Z-win64.zip` instead.
2. Close Notepad++.
3. Copy `NppSync.dll` (and `deps\`, if the ZIP has that folder) into
   `plugins\NppSync` under the folder that contains `notepad++.exe`.
   Create `plugins\NppSync` if it is not there.
   - Normal install: `C:\Program Files\Notepad++\plugins\NppSync\`
   - Installed for you only: the same `plugins\NppSync` folder inside
     that Notepad++, often
     `%LOCALAPPDATA%\Programs\Notepad++\plugins\NppSync\`
4. Open Notepad++. You should see **Plugins → Notepad++ Sync**.

> Windows may mark a DLL from the ZIP as blocked. If the plugin does not
> appear, right-click `NppSync.dll` → Properties → check **Unblock** → OK.
> The setup program writes the plugin itself, so that step is only for the ZIP.

## 2. First-run setup

A fresh install connects to `https://sync.berkkarabacak.com`. You do not
type a server address.

The first time the plugin loads, a short setup runs:

1. **Sign in with Google** (the primary button). Use the same Google account
   on every computer. Google only identifies the account. It is not the
   encryption key, and the plugin never stores a Google token. Email and
   password remain for an account you already created that way.
2. **Are your notes already on another computer?**
   - **No** — this is the first computer. An encryption key is created here
     and is not uploaded. You will see a recovery key like
     `NPSYNC-XXXX-…`. Keep it with this computer. **Typing that key on
     another computer does not open your notes.** It only unwraps a copy
     stored for this Windows user. If this computer is lost before another
     computer is allowed, the notes on the server cannot be read.
   - **Yes** — see [Adding a second computer](#5-adding-a-second-computer).
     This computer waits and does **not** create its own key.
3. **Name this device** (e.g. `Laptop-Home`) so you recognize it later.
4. **Choose files/folders to sync.**
5. Done, once this computer has the encryption key. Sync runs in the
   background from then on.

To use a server you run yourself, open **Plugins → Notepad++ Sync →
Settings → Advanced**, set **Backend URL** (for example
`http://localhost:8080`), choose **Save**, and restart Notepad++.
**Save** writes the address. The copy of Notepad++ that is already
running keeps the address it read at startup. See
[Self-hosting](self-hosting.md).

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

## 5. Adding a second computer

A second computer means another PC, or another Windows user on this PC.
Two Notepad++ windows under the same Windows user are **one** device: they
share `%APPDATA%\Notepad++Sync\`. Opening a second window does not copy the
key, and that window cannot allow itself.

You still click on each computer. You type one short code. You do not type
a web address.

1. On the **new** computer, install the plugin and start Notepad++.
2. In the setup window, choose **Yes**, then **Sign in with Google** with
   the same Google account.
3. When asked whether your notes are already on another computer, choose
   **Yes**. A window stays open and shows a code like `ABCD-EFGH` (about
   5 minutes). Leave it open. This computer does not create a new
   encryption key while it waits.
4. On the **computer that already has the notes**, choose
   **Plugins → Notepad++ Sync → Allow another computer** and type that
   code. That computer wraps its encryption key so the server cannot read
   it, and sends the wrapped key.
5. The window on the new computer closes by itself when the key arrives.
   Name the computer, choose the folder, and the notes decrypt there after
   the next sync.

If you closed the waiting window, or an older version of the plugin already
created a different key on the new computer: on the new computer choose
**Plugins → Notepad++ Sync → Get my notes**, leave the code on screen, and
do step 4 on the computer that already has the notes. If a key is already
there, **Get my notes** asks before replacing it.

**Allow another computer** on the computer that is showing the code fails
on purpose. That computer asked for the code. The computer that already
has the notes is the one that types it.

The recovery key shown on the first computer does not unlock the second.

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

Open Windows **Settings → Apps** and uninstall **Notepad++ Sync**, or run
`Uninstall.exe` in the `plugins\NppSync` folder. If Windows asks for
permission, choose **Yes**. If Notepad++ is open, the uninstaller asks
before it closes it. Choose **Yes** only when you are ready for Notepad++
to ask you to save.

This removes the plugin only. Settings, the queue, logs, and the encryption
key stay in `%APPDATA%\Notepad++Sync\`. Delete that folder only when you
mean to erase this computer's copy of the key. Your encrypted files remain
on the server until you delete your account.
