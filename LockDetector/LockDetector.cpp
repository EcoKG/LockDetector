#include <windows.h>
#include <stdlib.h>
#include <wchar.h>
#include "LockHooks.h"
#include "LockTracker.h"
#include "../Common/LockProtocol.h"

static HMODULE g_module = NULL;
extern "C" __declspec(dllexport) BOOL WINAPI LockDetectorStart(LPCWSTR logPath) {
    LD_InitializeTracker();
    if (logPath && *logPath) LD_SetLogPath(logPath);
    LD_LogMessage("INFO", "LockDetector starting");
    return LD_InstallHooks();
}
extern "C" __declspec(dllexport) void WINAPI LockDetectorStop() {
    LD_RemoveHooks();
    LD_LogMessage("INFO", "LockDetector stopped");
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
