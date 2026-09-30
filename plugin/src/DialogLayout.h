// DialogLayout.h — sizes for the plugin's Win32 dialogs.
//
// User-visible text is plain ASCII. An em dash, en dash, ellipsis, or
// arrow in a UTF-8 source file becomes mojibake on a typical Windows
// build: MSVC reads the source as the ANSI code page unless /utf-8 is
// set, so "Notepad++ Sync — Sign In" shows up as "Notepad++ Sync â€” Sign In".
// Hyphens, periods, and "..." stay readable without that flag.
//
// Controls are created in pixels. The dialog frame is a DLGTEMPLATE, so
// cx/cy are dialog units. For the template font (MS Shell Dlg, 8 pt) the
// base units at 96 DPI are about 6 by 13, not the system font's 8 by 16.
// Storing cx = pixelWidth / 2 therefore makes the client about 25 percent
// too narrow and clips labels. Convert with the real base units and a
// little pad:
//   pixelX = dluX * baseX / 4
//   dluX   = ceil(pixelX * 4 / baseX) + pad
#pragma once

namespace npsync
{
namespace layout
{

constexpr int kBaseX = 6;
constexpr int kBaseY = 13;
constexpr int kPadX = 8;
constexpr int kPadY = 8;

// 8 pt GUI font on a typical 96 DPI Windows install is about 9 px wide.
// Push buttons center their caption, so a tight width clips both ends
// ("Remove Selected" at 130 px showed "emove Selecte").
constexpr int kCharPx = 9;
constexpr int kPushChromePx = 32;
constexpr int kCheckChromePx = 24;
constexpr int kMinPushPx = 88;
constexpr int kCheckHeight = 22;

constexpr int textChars(const wchar_t* s) {
    int n = 0;
    if (s) {
        while (s[n])
            ++n;
    }
    return n;
}

constexpr bool isAscii(const wchar_t* s) {
    if (!s)
        return true;
    for (; *s; ++s) {
        if (static_cast<unsigned int>(*s) > 127)
            return false;
    }
    return true;
}

constexpr int toDlgX(int pixelW) {
    if (pixelW < 0)
        pixelW = 0;
    return (pixelW * 4 + kBaseX - 1) / kBaseX + kPadX;
}

constexpr int toDlgY(int pixelH) {
    if (pixelH < 0)
        pixelH = 0;
    return (pixelH * 8 + kBaseY - 1) / kBaseY + kPadY;
}

constexpr int fromDlgX(int dlu) {
    return dlu * kBaseX / 4;
}

constexpr int fromDlgY(int dlu) {
    return dlu * kBaseY / 8;
}

constexpr int pushButtonPx(const wchar_t* text) {
    int w = textChars(text) * kCharPx + kPushChromePx;
    return w < kMinPushPx ? kMinPushPx : w;
}

constexpr int checkBoxPx(const wchar_t* text) {
    return textChars(text) * kCharPx + kCheckChromePx;
}

constexpr int labelPx(const wchar_t* text) {
    return textChars(text) * kCharPx + 4;
}

// Former DialogMemory::begin stored cx = pixelWidth / 2. At the MS Shell
// Dlg base units that client is narrower than the pixel layout.
constexpr int legacyClientPx(int pixelW) {
    return (pixelW / 2) * kBaseX / 4;
}

struct DialogSize
{
    int pixelW;
    int pixelH;
    int contentRight;
    int contentBottom;
};

// contentRight / contentBottom are the pixel edges the controls reach.
// The converted dialog client must cover the whole pixel frame.
inline constexpr DialogSize kPrompt{560, 150, 544, 124};
inline constexpr DialogSize kSignIn{640, 320, 624, 288};
inline constexpr DialogSize kStatus{440, 360, 424, 344};
inline constexpr DialogSize kPair{600, 240, 584, 216};
inline constexpr DialogSize kDevices{600, 380, 584, 332};
inline constexpr DialogSize kSyncedFiles{640, 460, 624, 424};
inline constexpr DialogSize kConflicts{660, 340, 644, 300};
inline constexpr DialogSize kSettings{680, 350, 672, 324};

constexpr bool frameFits(const DialogSize& d) {
    return d.contentRight <= d.pixelW && d.contentBottom <= d.pixelH &&
           fromDlgX(toDlgX(d.pixelW)) >= d.pixelW && fromDlgY(toDlgY(d.pixelH)) >= d.pixelH &&
           fromDlgX(toDlgX(d.pixelW)) >= d.contentRight && fromDlgY(toDlgY(d.pixelH)) >= d.contentBottom;
}

constexpr const wchar_t kTitleSignIn[] = L"Notepad++ Sync - Sign In";
constexpr const wchar_t kTitleStatus[] = L"Notepad++ Sync - Status";
constexpr const wchar_t kTitlePair[] = L"Notepad++ Sync - Other computer";
constexpr const wchar_t kTitleDevices[] = L"Notepad++ Sync - Manage Devices";
constexpr const wchar_t kTitleSyncedFiles[] = L"Notepad++ Sync - Synced Files/Folders";
constexpr const wchar_t kTitleConflicts[] = L"Notepad++ Sync - Conflicts";
constexpr const wchar_t kTitleSettings[] = L"Notepad++ Sync - Settings";
constexpr const wchar_t kTitleRecovery[] = L"Notepad++ Sync - Recovery Key";
constexpr const wchar_t kTitleSetup[] = L"Notepad++ Sync - Setup";

constexpr const wchar_t kSignInLead[] = L"Sign in with Google. This only identifies your account -";
constexpr const wchar_t kCreateAccount[] = L"Create a new account (instead of signing in)";
constexpr const wchar_t kAddFolder[] = L"Add Folder...";
constexpr const wchar_t kAddFile[] = L"Add File...";
constexpr const wchar_t kRemoveSelected[] = L"Remove Selected";
constexpr const wchar_t kSavePatterns[] = L"Save Patterns";
constexpr const wchar_t kPairLead[] = L"On the computer that already has your notes, open Notepad++.";
constexpr const wchar_t kPairMenu[] = L"Choose Plugins -> Notepad++ Sync -> Allow another computer.";

static_assert(isAscii(kTitleSignIn), "sign-in title must be ASCII");
static_assert(isAscii(kTitleStatus), "status title must be ASCII");
static_assert(isAscii(kTitlePair), "pairing title must be ASCII");
static_assert(isAscii(kTitleDevices), "devices title must be ASCII");
static_assert(isAscii(kTitleSyncedFiles), "synced-files title must be ASCII");
static_assert(isAscii(kTitleConflicts), "conflicts title must be ASCII");
static_assert(isAscii(kTitleSettings), "settings title must be ASCII");
static_assert(isAscii(kTitleRecovery), "recovery-key title must be ASCII");
static_assert(isAscii(kTitleSetup), "setup title must be ASCII");
static_assert(isAscii(kSignInLead), "sign-in label must be ASCII");
static_assert(isAscii(kCreateAccount), "create-account label must be ASCII");
static_assert(isAscii(kAddFolder) && isAscii(kAddFile), "add buttons must be ASCII");
static_assert(isAscii(kRemoveSelected) && isAscii(kSavePatterns), "synced-files buttons must be ASCII");
static_assert(isAscii(kPairLead) && isAscii(kPairMenu), "pairing labels must be ASCII");

static_assert(frameFits(kPrompt), "prompt dialog units do not cover its controls");
static_assert(frameFits(kSignIn), "sign-in dialog units do not cover its labels");
static_assert(frameFits(kStatus), "status dialog units do not cover its controls");
static_assert(frameFits(kPair), "pairing dialog units do not cover its labels");
static_assert(frameFits(kDevices), "devices dialog units do not cover its controls");
static_assert(frameFits(kSyncedFiles), "synced-files dialog units do not cover its buttons");
static_assert(frameFits(kConflicts), "conflicts dialog units do not cover its buttons");
static_assert(frameFits(kSettings), "settings dialog units do not cover its labels");

// Caption plus the close button. Sign in, Synced Files/Folders, Recovery
// Key, and Setup are the titles that were unreadable or clipped.
static_assert(labelPx(kTitleSignIn) + 96 <= kSignIn.pixelW, "sign-in caption is wider than the window");
static_assert(labelPx(kTitleSyncedFiles) + 96 <= kSyncedFiles.pixelW,
              "synced-files caption is wider than the window");
static_assert(labelPx(kTitleRecovery) < 400, "recovery-key title must stay short");
static_assert(labelPx(kTitleSetup) < 400, "setup title must stay short");

static_assert(16 + labelPx(kSignInLead) <= kSignIn.pixelW - 16, "sign-in lead label does not fit");
static_assert(16 + labelPx(kPairLead) <= kPair.pixelW - 16, "pairing lead label does not fit");
static_assert(16 + labelPx(kPairMenu) <= kPair.pixelW - 16, "pairing menu label does not fit");
static_assert(labelPx(L"Type the code shown on the other computer (ABCD-EFGH):") <= kPrompt.pixelW - 32,
              "pairing-code prompt label does not fit");
static_assert(pushButtonPx(kRemoveSelected) > 130, "Remove Selected used to clip at 130 px");
static_assert(pushButtonPx(kAddFolder) > 110, "Add Folder used to clip at 110 px");
static_assert(pushButtonPx(kAddFile) > 110, "Add File used to clip at 110 px");
static_assert(pushButtonPx(kSavePatterns) > 110, "Save Patterns used to clip at 110 px");
static_assert(checkBoxPx(kCreateAccount) > 314, "create-account checkbox used to clip at 314 px");

// The old divide-by-2 client is still too narrow for these labels, which
// is why the pixel frame itself was widened and the unit conversion fixed.
static_assert(legacyClientPx(kSignIn.pixelW) < 16 + labelPx(kSignInLead),
              "legacy dialog units must be shown to clip the sign-in label");
static_assert(fromDlgX(toDlgX(kSignIn.pixelW)) >= 16 + labelPx(kSignInLead),
              "sign-in dialog units must cover the lead label");
static_assert(legacyClientPx(kSignIn.pixelW) < 124 + checkBoxPx(kCreateAccount),
              "legacy dialog units must be shown to clip the create-account checkbox");
static_assert(fromDlgX(toDlgX(kSignIn.pixelW)) >= 124 + checkBoxPx(kCreateAccount),
              "sign-in dialog units must cover the create-account checkbox");

} // namespace layout
} // namespace npsync
