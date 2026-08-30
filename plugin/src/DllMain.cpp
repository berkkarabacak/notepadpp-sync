// DllMain.cpp — DLL entry point and the extern "C" exports Notepad++
// expects from a plugin (see the official Notepad++ plugin template).
#include "PluginDefinition.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

BOOL APIENTRY DllMain(HANDLE hModule, DWORD reasonForCall, LPVOID /*reserved*/) {
    switch (reasonForCall) {
    case DLL_PROCESS_ATTACH:
        npsync::pluginInit(reinterpret_cast<HINSTANCE>(hModule));
        break;
    case DLL_PROCESS_DETACH:
        npsync::pluginCleanup();
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void setInfo(NppData notpadPlusData) {
    npsync::pluginSetInfo(notpadPlusData);
}

extern "C" __declspec(dllexport) const wchar_t* getName() {
    return npsync::kPluginName;
}

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* nbF) {
    return npsync::pluginGetFuncsArray(nbF);
}

extern "C" __declspec(dllexport) void beNotified(SCNotification* notify) {
    npsync::pluginBeNotified(notify);
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT msg, WPARAM wParam, LPARAM lParam) {
    return npsync::pluginMessageProc(msg, wParam, lParam);
}

extern "C" __declspec(dllexport) BOOL isUnicode() {
    return TRUE; // Notepad++ ≥ 7.x requires the Unicode interface
}
