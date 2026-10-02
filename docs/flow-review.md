# Flow review — install, pair, edit, close, see it on the other computer

Scope: critique of the demo flow only. No new features. Findings come from reading
the docs and `plugin/src` (branch `cursor/fix-close-hang-e36c`). Nothing here was
timed on two real computers, so every delay is derived from code, not measured.

Flow reviewed: install → sign in → pair second computer → open note → edit/save →
close Notepad++ → note appears on the other computer.

## Problem 1 — Save then close quickly leaves the note on the first computer

**Where:** `SyncEngine.cpp` `workerLoop()` (~line 1370), `stop()` (~line 102).

**What happens**
- A save calls `queueUpload`, which writes an op to the local SQLite queue and
  sets `syncRequested_ = true`.
- `syncRequested_` is never read anywhere (`grep` shows only assignments). The
  worker thread only checks the queue after a fixed sleep of 10 × 500 ms (about 5 s).
- So an upload starts up to ~5 s after the save. `syncNow()` also only sets this
  flag, so **Sync Now** does nothing immediately.
- On close, `stop()` sets `running_ = false` and calls `api_->cancelRequests()`
  (added to fix a close hang). A queued op that has not started never runs. An
  upload in flight is cancelled. The op stays in the queue and only uploads the
  next time Notepad++ is opened on that computer.

**What the user sees:** Save, close, open the other computer, no note. No message
on either side. It looks like the note was lost.

**Smallest fix**
1. In `workerLoop`, replace the fixed 10 × 500 ms sleep with a loop that exits
   early when `syncRequested_` is true, and clear the flag when it fires.
2. In `stop()`, before cancelling requests, give the queue one bounded pass (for
   example up to 2–3 s) so a just-saved file is uploaded. Keep the cancel as the
   fallback so close cannot hang again.

## Problem 2 — Pairing wait looks frozen and ends with an empty second computer

**Where:** `Dialogs.cpp` `pairWaitProc` (~line 497), `SyncEngine.cpp`
`completePairing` (~line 389).

**What happens**
- The waiting dialog polls from a `WM_TIMER` every 1.5 s. Each poll calls
  `completePairing` → `api_->pairPoll`, a blocking network call on the Notepad++
  UI thread. On a slow or dead connection the UI freezes for up to the HTTP
  connect timeout (10 s) per poll.
- The only feedback is static text, "Waiting for the other computer...", for up to
  5 minutes. No countdown, no sign it is alive.
- When the key arrives the dialog closes and says notes appear "after the next
  sync". Nothing triggers a sync, so the second computer can look empty for up to
  the 30 s poll interval (`syncIntervalFallbackSec`).

**Smallest fix**
- When pairing installs the key, call `syncNow()` (works once Problem 1 is fixed)
  and change the closing message to something like "Downloading your notes now…".
- Add the seconds remaining to the waiting text ("Waiting… code valid for 4:32")
  so the window visibly changes.
- Optional, cheaper than moving the poll off the UI thread: lower the connect
  timeout used by `pairPoll`.

## Problem 3 — Status says "Synced" when nothing has synced

**Where:** `SyncEngine.cpp` `start()` (~line 90), `pollerLoop()` (~line 1394),
`onRemoteFileApplied` in `PluginDefinition.cpp`.

**What happens**
- `start()` sets status "Ready" (shown as Synced) as soon as tokens exist, before
  any sync.
- The poller sets "Synced" after a pull whenever the queue is empty, and the
  worker sets it from a different thread. The user can see Synced while an
  upload has not run yet (Problem 1).
- A remote update only reloads the buffer if the file is the **active** tab
  (`currentFilePath()` comparison). A note open in a background tab is replaced on
  disk with no plugin message, so the open text can look stale or "reverted".

**Smallest fix**
- Show Synced only when the pending-op count is 0 and the last pull succeeded.
  Otherwise show "N changes waiting to upload".
- Run the same open-file check against every open tab, not only the current one.

## Fix order

1. Problem 1 (data appears lost; can fail a demo at the close step).
2. Problem 3 (one condition once Problem 1 is in).
3. Problem 2 (mostly wording plus one `syncNow()` call after pairing).

## Acceptance check (manual, two computers or two Windows users)

1. Save a note, close Notepad++ within 2 s. Open the other computer. The note is
   there within one poll or push.
2. Click **Sync Now** after a save. Upload starts in under a second.
3. Pair a second computer on a slow link. Notepad++ stays responsive; the dialog
   text changes while waiting; notes begin downloading right after the key
   installs.
4. With a queued upload, the status does not read Synced.
5. A note open in a background tab updates after a remote edit.
