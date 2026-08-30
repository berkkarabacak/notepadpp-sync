// Notepad_plus_msgs.h — minimal subset of Notepad++ message constants used
// by this plugin (from the official Notepad++ plugin template).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define NPPMSG (WM_USER + 1000)

#define NPPM_GETCURRENTBUFFERID   (NPPMSG + 60)
#define NPPM_GETNBOPENFILES       (NPPMSG + 7)
#define NPPM_GETOPENFILENAMES     (NPPMSG + 8)
#define NPPM_GETFULLPATHFROMBUFFERID (NPPMSG + 58)
#define NPPM_GETPOSFROMBUFFERID   (NPPMSG + 57)
#define NPPM_RELOADFILE           (NPPMSG + 40)
#define NPPM_SWITCHTOFILE         (NPPMSG + 37)
#define NPPM_GETCURRENTNATIVELANGENCODING (NPPMSG + 79)
#define NPPM_GETBUFFERENCODING    (NPPMSG + 65)
#define NPPM_SETMENUITEMCHECK     (NPPMSG + 22)

// Buffer IDs for the two views.
#define MAIN_VIEW 0
#define SUB_VIEW  1

// ---- plugin notifications (SCNotification.code, nmhdr.code base) ----
#define NPPN_FIRST 1000

// NPPN_READY: Notepad++ has completed startup; safe to initialize.
#define NPPN_READY (NPPN_FIRST + 1)
// NPPN_SHUTDOWN: Notepad++ is about to exit.
#define NPPN_SHUTDOWN (NPPN_FIRST + 9)
// NPPN_BUFFERACTIVATED: a buffer was activated.
#define NPPN_BUFFERACTIVATED (NPPN_FIRST + 10)
// NPPN_FILEBEFORESAVE / NPPN_FILESAVED: save lifecycle.
#define NPPN_FILEBEFORESAVE (NPPN_FIRST + 18)
#define NPPN_FILESAVED (NPPN_FIRST + 19)
// NPPN_FILEOPENED / NPPN_FILECLOSED: open/close lifecycle.
#define NPPN_FILEOPENED (NPPN_FIRST + 15)
#define NPPN_FILECLOSED (NPPN_FIRST + 17)
// NPPN_FILEDELETED: file deleted externally.
#define NPPN_FILEDELETED (NPPN_FIRST + 26)
// NPPN_FILERENAMED: file renamed externally.
#define NPPN_FILERENAMED (NPPN_FIRST + 27)

// ---- Scintilla messages used (values from Scintilla.h) ----
#define SCI_GETTEXT 2182
#define SCI_GETTEXTLENGTH 2183
#define SCI_GETCURRENTPOS 2008
#define SCI_GOTOPOS 2025
#define SCI_GETFIRSTVISIBLELINE 2152
#define SCI_SETFIRSTVISIBLELINE 2613
#define SCI_GRABFOCUS 2400
