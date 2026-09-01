# Acceptance Test Script — Notepad++ Sync v1.0.0

Run this before announcing the release publicly. It verifies the promises in
the README on real machines. Expected total time: ~45 minutes.

**Test rig:** two Windows machines (or two Windows user accounts / VMs) with
Notepad++ ≥ 8.x, plus a running server (`docker compose up -d` or the dev
server from `docs/developer.md`). Both plugin instances sign into the **same
account** as two devices (`TestLaptop-A`, `TestLaptop-B`).

**How to record results:** check the box and note the time. If a step fails,
copy the last 50 lines of `%APPDATA%\Notepad++Sync\logs\npsync-YYYYMM.log`
into the issue you file.

---

## 0. Install & first run

- [ ] **0.1** Fresh install: copy `NppSync.dll` to `plugins\NppSync\` on both
      machines, start Notepad++, menu **Plugins → Notepad++ Sync** exists.
- [ ] **0.2** First-run wizard appears on A: create account
      `acceptance@test.dev`, device name `TestLaptop-A`.
- [ ] **0.3** Recovery key `NPSYNC-…` is displayed once; write it down.
- [ ] **0.4** On B: sign in to the same account, device name `TestLaptop-B`.
      Use the **pairing code flow** (request on B, approve on A) — keys
      transfer without typing the recovery key.
- [ ] **0.5** Add sync folder `C:\SyncTest` on both machines.

## 1. Basic propagation

- [ ] **1.1** On A: create `C:\SyncTest\hello.txt` with `line from A`, save.
      Within ~5 s the file appears on B with identical content.
- [ ] **1.2** On B: append `line from B`, save. It appears on A.
- [ ] **1.3** Sync Status on both shows `Synced`, 0 pending, 0 conflicts.
- [ ] **1.4** Create a subfolder with a file on A (`C:\SyncTest\sub\note.txt`)
      — propagates with folder structure intact.
- [ ] **1.5** Rename `hello.txt` → `hello-renamed.txt` on A — B converges
      (rename or delete+create; no duplicate left behind after one cycle).
- [ ] **1.6** Delete `hello-renamed.txt` on A — deleted on B; the file shows
      a tombstone (`deleted: true`) via `GET /sync/files` if you check the API.

## 2. Offline queue & reconnect

- [ ] **2.1** Disable networking on B. Edit `hello.txt`-successor file
      `offline.txt` on B (three saves, rapid). Status shows `Offline`.
- [ ] **2.2** Fully exit Notepad++ on B, reboot B (queue must survive).
- [ ] **2.3** Re-enable networking, start Notepad++. Within ~30 s, A receives
      the final content — exactly once (no duplicate versions in history
      beyond real saves).
- [ ] **2.4** Rapid-save test: hold Ctrl+S ~20 times on A. Version history on
      the server shows coalesced uploads, not 20 identical versions.

## 3. Conflict handling (the core promise)

- [ ] **3.1** Disconnect **both** from the network. On A set `conflict.txt`
      to `A's edit`; on B set it to `B's edit`. Save both.
- [ ] **3.2** Reconnect A, wait for upload. Reconnect B.
- [ ] **3.3** Expected: conflict detected. B (or A, whoever uploads second)
      shows status `Conflict`. Open **Conflicts** — the file is listed.
- [ ] **3.4** Choose **Keep Both** on the conflicted machine. Verify:
      canonical file has one version, and a
      `conflict (conflict - TestLaptop-X - YYYY-MM-DD).txt` copy exists with
      the other version. **Nothing is lost.**
- [ ] **3.5** Repeat 3.1–3.2 with edits in **different paragraphs**. Expected:
      automatic three-way merge succeeds; merged file contains both edits;
      no conflict is raised.
- [ ] **3.6** Server check: `GET /sync/files/{id}/versions` shows every
      accepted version; the losing conflict payload remains retrievable.

## 4. Ignore rules & file selection

- [ ] **4.1** Create `.npsyncignore` in `C:\SyncTest` with `*.tmp` and
      `scratch/`. Create `x.tmp` and `scratch\note.txt` on A. They must
      **not** appear on B or in `GET /sync/files`.
- [ ] **4.2** Remove the folder from sync roots on A; files stay on disk
      locally; no mass-deletion happens on B.

## 5. Security spot-checks (server side)

- [ ] **5.1** In PostgreSQL: `SELECT encrypted_metadata, blob_key FROM files;`
      — all values are binary/opaque. No plaintext filename or note content
      anywhere in `files`, `file_versions`, or the blob files on disk
      (`docker compose exec db ...` / browse the blobs volume).
- [ ] **5.2** `SELECT password_hash FROM accounts;` — Argon2id strings only.
- [ ] **5.3** Try 9 wrong-password logins — account locks (429). Wait or
      clear, then correct password works.
- [ ] **5.4** Revoke `TestLaptop-B` from A (Manage Devices). B's next sync
      fails with 401; B's WebSocket is dropped; B's refresh token is dead.
- [ ] **5.5** Wrong protocol header: send `X-NPSync-Protocol: 99` — server
      replies 426 with `unsupported_protocol`.
- [ ] **5.6** Upload a >100 MB file — rejected 413.

## 6. Session sync (optional feature)

- [ ] **6.1** Enable *Sync files + tabs* on both. Open 3 tabs on A, one
      synced. On B, the synced tab state arrives (check
      `GET /session` contains an encrypted blob).
- [ ] **6.2** Confirm an **unsaved** `new 1` tab on A never produces any
      server-side record.

## 7. Resilience

- [ ] **7.1** Kill the server mid-upload (`docker compose stop server`).
      Plugin A shows Offline; restart server; sync resumes without manual
      intervention.
- [ ] **7.2** Kill Notepad++ (Task Manager) mid-sync. Restart — state
      consistent, queue intact, no corrupt `sync.db`.
- [ ] **7.3** Restore an older version from version history — file reverts
      locally and propagates as a new head version.

---

**Sign-off:** all boxes checked on ______ (date) by ______.
File issues at: https://github.com/berkkarabacak/notepadpp-sync/issues
