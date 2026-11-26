#pragma once
#include <windows.h>

enum LD_LOCK_KIND {
    LD_CRITICAL_SECTION = 1,
    LD_MUTEX = 2,
    LD_SRW_EXCLUSIVE = 3,
    LD_SRW_SHARED = 4,
    LD_STL_WRAPPER = 5
};

struct LD_Thread;   // per-thread tracker state (fixed pool, no heap)

// ---- lifecycle -----------------------------------------------------------
BOOL LD_InitializeTracker();            // allocates fixed tables; idempotent
BOOL LD_StartLogger();                  // opens the log file, starts the writer thread
void LD_StopLogger();                   // drains the ring buffer and stops the writer
void LD_SetSelfModule(HMODULE self);    // from DllMain
void LD_ThreadExit();                   // from DllMain(DLL_THREAD_DETACH)
void LD_SetLogPath(const wchar_t* path);
void LD_LogMessage(const char* level, const char* message);
void LD_LogStats();

// ---- hook-facing API (never blocks on OS locks, never allocates) ----------
// Returns NULL when the call must be passed through untracked: tracker not ready,
// re-entrant call from inside the tracker, or a caller located in a system module
// (loader, heap, CRT internals ...). 'returnAddress' == NULL means "treat as user code".
LD_Thread* LD_Begin(void* returnAddress);
void LD_End(LD_Thread* t);

// Called BEFORE a blocking acquire: records held->wanted order edges and reports a
// cycle immediately, i.e. before the thread can block forever.
void LD_BeforeAcquire(LD_Thread* t, void* object, int kind, const char* op);
// Called after the acquire succeeded. checkOrder=true for try-acquire (no pre-call).
void LD_AfterAcquire(LD_Thread* t, void* object, int kind, const char* op, bool checkOrder);
void LD_Release(LD_Thread* t, void* object, int kind, const char* op);
void LD_ObjectDestroyed(void* object, int kind);

// Mutex handle registry (only handles created after start are known).
void LD_RegisterMutex(HANDLE h);
bool LD_IsMutex(HANDLE h);
void LD_UnregisterMutex(HANDLE h);

// ---- compatibility wrappers (original API; also usable by an STL wrapper) --
void LD_TrackAcquire(void* object, int kind, const char* operation);
void LD_TrackRelease(void* object, int kind, const char* operation);
