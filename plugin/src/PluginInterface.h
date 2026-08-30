// PluginInterface.h — minimal subset of the official Notepad++ plugin
// interface (from the Notepad++ Plugin Template, GPL-compatible use as
// interface definition). Only the structures this plugin needs are kept.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

const int nbChar = 64;

typedef const wchar_t* (__cdecl* PFUNCGETNAME)();

struct NppData {
    HWND _nppHandle;
    HWND _scintillaMainHandle;
    HWND _scintillaSecondHandle;
};

typedef void (__cdecl* PFUNCSETINFO)(NppData);
typedef void (__cdecl* PFUNCPLUGINCMD)();
typedef void (__cdecl* PBEOTIFIED)(void*);

struct ShortcutKey {
    bool _isCtrl = false;
    bool _isAlt = false;
    bool _isShift = false;
    UCHAR _key = 0;
};

struct FuncItem {
    wchar_t _itemName[nbChar] = {0};
    PFUNCPLUGINCMD _pFunc = nullptr;
    int _cmdID = 0;
    bool _init2Check = false;
    ShortcutKey* _pShKey = nullptr;
};

// Scintilla notification header (from Scintilla.iface).
struct Sci_NotifyHeader {
    HWND hwndFrom;
    uintptr_t idFrom;
    unsigned int code;
};

struct SCNotification {
    Sci_NotifyHeader nmhdr;
    intptr_t position;
    int ch;
    int modifiers;
    int modificationType;
    const char* text;
    intptr_t length;
    intptr_t linesAdded;
    int message;
    uintptr_t wParam;
    intptr_t lParam;
    intptr_t line;
    int foldLevelNow;
    int foldLevelPrev;
    int margin;
    int listType;
    int x;
    int y;
    int token;
    intptr_t annotationLinesAdded;
    int updated;
    int listCompletionMethod;
    int characterSource;
};
