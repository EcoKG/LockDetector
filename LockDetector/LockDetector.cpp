#include <windows.h>
#include "LockHooks.h"
#include "LockTracker.h"
#include "../Common/LockProtocol.h"

static volatile LONG g_started = 0;

// logPath: absolute path of the log file. NULL/empty falls back to the LOCKDETECTOR_LOG
// environment variable (the viewer uses the same variable) and then to LockDetector.log.
extern "C" __declspec(dllexport) BOOL WINAPI LockDetectorStart(LPCWSTR logPath) {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return TRUE;   // already started
    if (!LD_InitializeTracker()) { g_started = 0; return FALSE; }
    LD_SetLogPath(logPath);
    if (!LD_StartLogger()) { g_started = 0; return FALSE; }
    LD_LogMessage("INFO", "LockDetector starting");
    if (!LD_InstallHooks()) {
        LD_LogMessage("ERROR", "hook installation failed");
        LD_StopLogger();
        g_started = 0;
        return FALSE;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void WINAPI LockDetectorStop() {
    if (InterlockedCompareExchange(&g_started, 0, 1) != 1) return;
    LD_RemoveHooks();
    LD_LogStats();
    LD_LogMessage("INFO", "LockDetector stopped");
    LD_StopLogger();
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: LD_SetSelfModule(h); break;
    case DLL_THREAD_DETACH:  LD_ThreadExit(); break;
    }
    return TRUE;
}
