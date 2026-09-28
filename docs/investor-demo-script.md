# Investor demo script — Notepad++ Sync

A live path of about 8 minutes once the laptop is prepared. Starting
`docker compose` in the room can push it to 10. This is the short
script. The release checklist is
[acceptance-test-script.md](acceptance-test-script.md) (~45 minutes). Server
setup is the [Investor demo](self-hosting.md#investor-demo-one-laptop)
section of [self-hosting.md](self-hosting.md).

Say only what you just did. This script syncs a file on one device and
opens **Manage Devices → Pair new device** so the room can see a code.
Do not click **Approve pairing** on this PC: the server rejects it with
`cannot approve your own pairing request`. The menu never calls the poll
that installs a wrapped key (`completePairing`). Two Notepad++ windows
under the same Windows user are one device — they share
`%APPDATA%\Notepad++Sync\`. The two-device checklist is
[acceptance-test-script.md](acceptance-test-script.md).

## Before you start (not on the clock)

1. Server, from [Investor demo (one laptop)](self-hosting.md#investor-demo-one-laptop):

   ```bash
   cp .env.example .env
   # POSTGRES_PASSWORD, TOKEN_SIGNING_KEY (openssl rand -hex 32),
   # BASE_URL=http://localhost:8080
   docker compose up -d --build
   curl -fsS http://localhost:8080/health
   ```

   Leave `GOOGLE_CLIENT_ID` and `GOOGLE_CLIENT_SECRET` empty unless you are
   doing the optional Google block below. A public hostname, if you have
   one already, is [Public demo URL](self-hosting.md#public-demo-url) — not
   a `*.trycloudflare.com` Quick Tunnel.

2. Plugin installed ([user guide](user-guide.md)): `NppSync.dll` in
   `%APPDATA%\Notepad++\plugins\NppSync\` (or
   `C:\Program Files\Notepad++\plugins\NppSync\`).

3. Point the plugin at the server **before** the live sign-in. The client
   reads Backend URL at startup. **Save** does not retarget the copy that
   is already running.

   - Start Notepad++. If **Notepad++ Sync — Setup** appears, choose **No**.
   - **Plugins → Notepad++ Sync → Settings → Advanced**.
   - **Backend URL:** `http://localhost:8080` (or your `https://` public
     URL, the same value as `BASE_URL`). The untouched default is
     `https://sync.example.com` and does not connect.
   - **Settings → Security → This device name:** `Laptop-A`.
   - **Save**, then quit Notepad++ completely.

   Creating an account sends that device name. The server rejects an empty
   one (`device_name required`, max 64 characters). The wizard asks for the
   name again after sign-in; that later prompt is stored locally. The name
   in **Manage Devices** is the one sent at sign-in, so set it first.

4. A folder you can show, for example `C:\SyncTest` (empty is fine).

5. An email you can type (it needs `@` and a dot in the domain) and a
   password of at least 10 characters with letters and digits. Do not reuse
   a personal password. Pick both before you walk in.

## 0. Server is up (~30 s)

`curl -fsS http://localhost:8080/health` should print `{"status":"ok"}`. One
sentence: the stack is the API and PostgreSQL (`docker compose`); file
bytes stay in the `blobdata` volume.

## 1. Backend URL (~30 s)

Start Notepad++. **Plugins → Notepad++ Sync → Settings → Advanced** and
show **Backend URL** is `http://localhost:8080` (or the public `https://`
URL). Close with **Cancel** so you do not change it. There is no hosted
service to sign up for.

## 2. Create the account (~2 min)

The welcome dialog **Notepad++ Sync — Setup** should be on screen. Choose
**Yes**. That dialog says a second device still needs pairing or the
recovery key. Pairing in this room is step 5 (show the code only). A
fresh install cannot type the recovery key in to unlock notes; the next
dialog is the on-screen warning, not a restore flow.

**Notepad++ Sync — Sign In:**

- **Email** and **Password**.
- Check **Create a new account (instead of signing in)**.
- **Sign in with email**.

Skip **Sign in with Google** unless the optional block below applies. If
Google is not configured, that button returns “Google sign-in is not
configured on this server.”

Then:

1. **Notepad++ Sync — Recovery Key** shows a key starting with `NPSYNC-`.
   The dialog says to store it offline, and that losing every device and
   this key means the notes cannot be recovered. There is no password
   reset that recovers them. Do not read the key out. **OK**.

   That sentence is the intended-design warning printed on the dialog.
   What the plugin stores is local: `recovery_key_display` and
   `recovery_wrapped` are DPAPI secrets for this Windows user
   (`%APPDATA%\Notepad++Sync\secrets\`). `unlockWithRecoveryKey` only
   unwraps that local `recovery_wrapped` blob, and no menu calls it. A
   new install has neither secret, and the first-run wizard generates a
   new master key instead. Do not tell the room that another PC can use
   the key on screen.
2. **Name this device.** Enter `Laptop-A` again. **OK**.
3. **Notepad++ Sync — Synced Files/Folders → Add Folder…** and pick the
   demo folder. **Close**.
4. **Setup complete.** **OK**.

## 3. Optional — Google, only if both credentials are set (~2 min, replaces step 2’s email form)

Do this only when `.env` has both `GOOGLE_CLIENT_ID` and
`GOOGLE_CLIENT_SECRET`, the server was restarted after that, and the Google
client’s authorized redirect URI is exactly
`${BASE_URL}/auth/google/callback` (or the `GOOGLE_REDIRECT_URI` you set).
Console steps: [Google sign-in](self-hosting.md#google-sign-in). While the
OAuth app is in **Testing**, the Google account must be listed as a test
user, or Google returns `access_denied`.

On **Notepad++ Sync — Sign In**, click **Sign in with Google**. The system
browser opens. Finish there. The dialog says “Waiting for Google… finish
in the browser, then return here.” Then the same recovery-key, device-name,
and **Add Folder…** steps as above.

Google only identifies the account. It is not an encryption key. Say that
in one line and move on.

## 4. Save a file (~1 min)

In the demo folder, create `hello.txt`, type a line, and save it in
Notepad++.

**Plugins → Notepad++ Sync → Sync Now**, then
**Plugins → Notepad++ Sync → Sync Status**. You want status `Synced`, and
pending uploads and downloads at 0. The acceptance script’s basic case
shows up within about 5 seconds; if the window still says pending, wait
one cycle or hit **Sync Now** again. **Close**.

## 5. Pairing story (~2 min)

Stay on this PC. **Plugins → Notepad++ Sync → Manage Devices**.

Click **Pair new device** only. The server returns a code and a dialog
shows it (shape `ABCD-EFGH`, valid 5 minutes, single-use, alphabet without
`0`/`O`/`1`/`I`/`L`). The dialog says to enter that code on a device that
already has the notes: **Manage Devices → Approve pairing**.

Do **not** click **Approve pairing** on this same PC. It will fail.
**Pair new device** recorded this device as the requester. Approving here
sends the same device id, and `pairApprove` rejects it when
`RequestingDevice` is the current device
(`server/internal/api/device_handlers.go`) with
`cannot approve your own pairing request`. The status line will not say
“Approved. The new device can now unlock its keys.”

Say what a second device that is already unlocked would do next. That
device is another Windows user, VM, or laptop, already signed in and
holding this account’s master key — not a second Notepad++ window on this
PC. There, **Manage Devices → Approve pairing** takes the code, wraps the
master key with AES-256-GCM under a key derived from the code, and uploads
that blob. The server stores it and cannot read it
([protocol — device pairing](../protocol/docs/protocol-v1.md#device-pairing-key-transfer)).

The menu does not call `completePairing` and does not poll
`POST /devices/pair` with `action: poll`, so the requesting device never
installs that wrapped key from the dialog, even after a real approval on
another device. The first-run wizard on a second PC also generates a new
local master key when that PC has none, which is not this PC’s key.

Google or a password does not carry the master key. The recovery key from
step 2 does not either.

Do not promise the note will show up on another PC in this sitting. The
full two-device path is
[acceptance-test-script.md](acceptance-test-script.md) (step 0.4 requests
on the new device and approves on a device that already has the notes).
Use it when you have that second Windows user, VM, or laptop — not this
10-minute script.

## 6. One security sentence (~30 s)

With **Sync Status** already shown, or while the file is open:

> The file was encrypted on this PC with AES-256-GCM before upload. The
> server stores ciphertext, an opaque file id, and encrypted names and
> paths. It never has the master key or the plaintext. The password, and
> Google if you used it, only sign the account in.

That is the claim in the [security model](security-model.md). If someone
asks to see it, the check is step 5.1 of the
[acceptance script](acceptance-test-script.md) (`encrypted_metadata` and
the blob volume are opaque). Do not open a password hash or a recovery key
on the projector.

## If something fails

| What you see | What to check |
|--------------|----------------|
| Cannot reach server | Backend URL, and that Notepad++ was restarted after **Save**. `curl http://localhost:8080/health`. |
| `device_name required` | **Settings → Security → This device name**, **Save**, restart, create the account again. |
| Google button errors | Both `GOOGLE_CLIENT_ID` and `GOOGLE_CLIENT_SECRET` set, redirect URI matches `BASE_URL`, test user listed. Otherwise use email. |
| File never leaves the laptop | A folder was added under **Synced Files/Folders**, and the file is inside it. **Sync Now**. |
| You expected the file on a second PC | This script does not do that. See step 5. The long path is the acceptance script. |
| **Approve pairing** on this PC | Expected failure: `cannot approve your own pairing request`. Leave that button alone. See step 5. |

Logs, if you need them after the room:
`%APPDATA%\Notepad++Sync\logs\npsync-YYYYMM.log`.
