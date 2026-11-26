#pragma once
#include <windows.h>

enum LD_LOCK_KIND {
    LD_CRITICAL_SECTION = 1,
    LD_MUTEX = 2,
    LD_SRW_EXCLUSIVE = 3,
    LD_SRW_SHARED = 4,
    LD_STL_WRAPPER = 5
};

void LD_TrackAcquire(void* object, int kind, const char* operation);
void LD_TrackRelease(void* object, int kind, const char* operation);
void LD_LogMessage(const char* level, const char* message);
void LD_SetLogPath(const wchar_t* path);
void LD_InitializeTracker();
