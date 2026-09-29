# Investor demo script — Notepad++ Sync

A live path of about 8 minutes once the laptop is prepared. Starting
`docker compose` in the room can push it to 10. This is the short
script. The release checklist is
[acceptance-test-script.md](acceptance-test-script.md) (~45 minutes). Server
setup is the [Investor demo](self-hosting.md#investor-demo-one-laptop)
section of [self-hosting.md](self-hosting.md).

Say only what you just did. This script syncs a file on one device. A
second computer is a second Windows user, VM, or PC — two Notepad++
windows under the same Windows user share `%APPDATA%\Notepad++Sync\` and
are one device. The clicks that move the encryption key are in the
[user guide](user-guide.md#5-adding-a-second-computer). The checklist is
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
     URL, the same value as `BASE_URL`). A fresh install uses
     `https://sync.berkkarabacak.com`. This laptop demo replaces that.
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
URL you set as `BASE_URL`). A fresh install would show
`https://sync.berkkarabacak.com`; this demo overrides that before
sign-in. Close with **Cancel** so you do not change it.

## 2. Create the account (~2 min)

The welcome dialog **Notepad++ Sync — Setup** should be on screen. Choose
**Yes**. It says you do not type a server address. For this room you
already set Backend URL in the step above, before the wizard.

**Notepad++ Sync — Sign In:**

- **Email** and **Password**.
- Check **Create a new account (instead of signing in)**.
- **Sign in with email**.

Skip **Sign in with Google** unless the optional block below applies. If
Google is not configured, that button returns “Google sign-in is not
configured on this server.”

Then setup asks **Are your notes already on another computer?** Choose
**No** — this is the first computer.

1. **Notepad++ Sync — Recovery Key** shows a key starting with `NPSYNC-`.
   The dialog says this key only unwraps the copy stored for this Windows
   user, and that typing it on another computer does not open the notes.
   That is accurate. `recovery_key_display` and `recovery_wrapped` are
   DPAPI secrets under `%APPDATA%\Notepad++Sync\secrets\`. They are not
   uploaded. Do not read the key out. **OK**. Do not tell the room that
   another PC can use the key on screen.
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
in the browser, then return here.” Then choose **No** (this is the first
computer), and the same recovery-key, device-name, and **Add Folder…**
steps as above.

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

## 5. What a second computer does (~1 min, say it; do the clicks only if that computer is in the room)

Stay on this PC unless a second Windows user, VM, or laptop is signed in
as the same account and is waiting on **Get my notes**.

On that other computer the setup question is **Yes** (notes are already
on another computer). A code like `ABCD-EFGH` stays on screen for about
5 minutes. That window calls `completePairing` until this PC allows it.
The wizard does not mint a second master key while it waits, and it does
not mint after the wrapped key is installed.

On **this** PC: **Plugins → Notepad++ Sync → Allow another computer**,
type the code. This PC wraps the master key with AES-256-GCM under a key
derived from the code and uploads that blob. The server stores it and
cannot read it
([protocol — device pairing](../protocol/docs/protocol-v1.md#device-pairing-key-transfer)).
The other window then finishes by itself.

Do **not** choose **Allow another computer** on the computer that is
showing the code. The server rejects it with
`cannot approve your own pairing request`. The dialog explains that in
plain language. Two Notepad++ windows here are still one device.

If you do not have that second computer in the room, say the paragraph
above and move on. Do not start **Get my notes** on this PC and then try
to allow it here. Google, the password, and the recovery key from step 2
do not carry the master key. The click-by-click list is
[Adding a second computer](user-guide.md#5-adding-a-second-computer).

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
| You expected the file on a second PC | That computer must be waiting on **Get my notes**, and this one must choose **Allow another computer**. See step 5 and the user guide. |
| **Allow another computer** on the PC that is showing the code | Expected failure. That PC created the code. Allow it from the PC that already has the notes. |

Logs, if you need them after the room:
`%APPDATA%\Notepad++Sync\logs\npsync-YYYYMM.log`.
