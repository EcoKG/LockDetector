#include "LockHooks.h"
#include "LockTracker.h"
#include <detours.h>
#include <intrin.h>
#include <tlhelp32.h>

// Every hook follows the same shape:
//   1. remember GetLastError (the tracker must not disturb the caller's error state)
//   2. LD_Begin(returnAddress): NULL => untracked caller / re-entrancy => plain pass-through
//   3. LD_BeforeAcquire (blocking acquires only: records order edges BEFORE we may block)
//   4. call the real function
//   5. LD_AfterAcquire / LD_Release, then restore the last-error of the real call

typedef VOID (WINAPI *PFN_EnterCriticalSection)(LPCRITICAL_SECTION);
typedef BOOL (WINAPI *PFN_TryEnterCriticalSection)(LPCRITICAL_SECTION);
typedef VOID (WINAPI *PFN_LeaveCriticalSection)(LPCRITICAL_SECTION);
typedef VOID (WINAPI *PFN_DeleteCriticalSection)(LPCRITICAL_SECTION);
typedef HANDLE (WINAPI *PFN_CreateMutexW)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
typedef HANDLE (WINAPI *PFN_CreateMutexA)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
typedef DWORD (WINAPI *PFN_WaitForSingleObject)(HANDLE, DWORD);
typedef DWORD (WINAPI *PFN_WaitForSingleObjectEx)(HANDLE, DWORD, BOOL);
typedef BOOL (WINAPI *PFN_ReleaseMutex)(HANDLE);
typedef BOOL (WINAPI *PFN_CloseHandle)(HANDLE);
typedef VOID (WINAPI *PFN_AcquireSRWLockExclusive)(PSRWLOCK);
typedef VOID (WINAPI *PFN_ReleaseSRWLockExclusive)(PSRWLOCK);
typedef VOID (WINAPI *PFN_AcquireSRWLockShared)(PSRWLOCK);
typedef VOID (WINAPI *PFN_ReleaseSRWLockShared)(PSRWLOCK);
typedef BOOLEAN (WINAPI *PFN_TryAcquireSRWLockExclusive)(PSRWLOCK);
typedef BOOLEAN (WINAPI *PFN_TryAcquireSRWLockShared)(PSRWLOCK);

static PFN_EnterCriticalSection RealEnterCS = EnterCriticalSection;
static PFN_TryEnterCriticalSection RealTryEnterCS = TryEnterCriticalSection;
static PFN_LeaveCriticalSection RealLeaveCS = LeaveCriticalSection;
static PFN_DeleteCriticalSection RealDeleteCS = DeleteCriticalSection;
static PFN_CreateMutexW RealCreateMutexW = CreateMutexW;
static PFN_CreateMutexA RealCreateMutexA = CreateMutexA;
static PFN_WaitForSingleObject RealWait = WaitForSingleObject;
static PFN_WaitForSingleObjectEx RealWaitEx = WaitForSingleObjectEx;
static PFN_ReleaseMutex RealReleaseMutex = ReleaseMutex;
static PFN_CloseHandle RealCloseHandle = CloseHandle;
static PFN_AcquireSRWLockExclusive RealAcquireEx = AcquireSRWLockExclusive;
static PFN_ReleaseSRWLockExclusive RealReleaseEx = ReleaseSRWLockExclusive;
static PFN_AcquireSRWLockShared RealAcquireSh = AcquireSRWLockShared;
static PFN_ReleaseSRWLockShared RealReleaseSh = ReleaseSRWLockShared;
static PFN_TryAcquireSRWLockExclusive RealTryEx = TryAcquireSRWLockExclusive;
static PFN_TryAcquireSRWLockShared RealTrySh = TryAcquireSRWLockShared;

// ---- critical sections ----------------------------------------------------------------
static VOID WINAPI HookEnterCS(LPCRITICAL_SECTION p) {
    void* ra = _ReturnAddress();
    DWORD e0 = GetLastError();
    LD_Thread* t = LD_Begin(ra);
    if (t) LD_BeforeAcquire(t, p, LD_CRITICAL_SECTION, "EnterCriticalSection");
    SetLastError(e0);
    RealEnterCS(p);
    if (t) { DWORD e1 = GetLastError(); LD_AfterAcquire(t, p, LD_CRITICAL_SECTION, "EnterCriticalSection", false); LD_End(t); SetLastError(e1); }
}
static BOOL WINAPI HookTryEnterCS(LPCRITICAL_SECTION p) {
    void* ra = _ReturnAddress();
    DWORD e0 = GetLastError();
    LD_Thread* t = LD_Begin(ra);
    SetLastError(e0);
    BOOL ok = RealTryEnterCS(p);
    if (t) { DWORD e1 = GetLastError(); if (ok) LD_AfterAcquire(t, p, LD_CRITICAL_SECTION, "TryEnterCriticalSection", true); LD_End(t); SetLastError(e1); }
    return ok;
}
static VOID WINAPI HookLeaveCS(LPCRITICAL_SECTION p) {
    void* ra = _ReturnAddress();
    DWORD e0 = GetLastError();
    LD_Thread* t = LD_Begin(ra);
    if (t) { LD_Release(t, p, LD_CRITICAL_SECTION, "LeaveCriticalSection"); LD_End(t); }
    SetLastError(e0);
    RealLeaveCS(p);
}
static VOID WINAPI HookDeleteCS(LPCRITICAL_SECTION p) {
    void* ra = _ReturnAddress();
    DWORD e0 = GetLastError();
    LD_Thread* t = LD_Begin(ra);
    if (t) { LD_ObjectDestroyed(p, LD_CRITICAL_SECTION); LD_End(t); }
    SetLastError(e0);
    RealDeleteCS(p);
}

// ---- mutexes -------------------------------------------------------------------------------
static HANDLE FinishCreateMutex(LD_Thread* t, HANDLE h, BOOL owner, const char* op) {
    if (!t) return h;
    DWORD e1 = GetLastError();
    if (h && h != INVALID_HANDLE_VALUE) {
        LD_RegisterMutex(h);
        if (owner && e1 != ERROR_ALREADY_EXISTS) LD_AfterAcquire(t, h, LD_MUTEX, op, true);
    }
    LD_End(t);
    SetLastError(e1);
    return h;
}
static HANDLE WINAPI HookCreateMutexW(LPSECURITY_ATTRIBUTES a, BOOL owner, LPCWSTR name) {
    void* ra = _ReturnAddress();
    DWORD e0 = GetLastError();
    LD_Thread* t = LD_Begin(ra);
    SetLastError(e0);
    HANDLE h = RealCreateMutexW(a, owner, name);
    return FinishCreateMutex(t, h, owner, "CreateMutexW(initial owner)");
}
static HANDLE WINAPI HookCreateMutexA(LPSECURITY_ATTRIBUTES a, BOOL owner, LPCSTR name) {
    void* ra = _ReturnAddress();
    DWORD e0 = GetLastError();
    LD_Thread* t = LD_Begin(ra);
    SetLastError(e0);
    HANDLE h = RealCreateMutexA(a, owner, name);
    return FinishCreateMutex(t, h, owner, "CreateMutexA(initial owner)");
}
static DWORD WINAPI HookWait(HANDLE h, DWORD ms) {
    void* ra = _ReturnAddress();
    DWORD e0 = GetLastError();
    LD_Thread* t = LD_Begin(ra);
    bool mtx = false;
    if (t) {
        mtx = LD_IsMutex(h);
        if (mtx && ms != 0) LD_BeforeAcquire(t, h, LD_MUTEX, "WaitForSingleObject");
        if (!mtx) { LD_End(t); t = NULL; }
    }
    SetLastError(e0);
    DWORD r = RealWait(h, ms);
    if (t) {
        DWORD e1 = GetLastError();
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) LD_AfterAcquire(t, h, LD_MUTEX, "WaitForSingleObject", ms == 0);
        LD_End(t); SetLastError(e1);
    }
    return r;
}
static DWORD WINAPI HookWaitEx(HANDLE h, DWORD ms, BOOL alertable) {
    void* ra = _ReturnAddress();
    DWORD e0 = GetLastError();
    LD_Thread* t = LD_Begin(ra);
    bool mtx = false;
    if (t) {
        mtx = LD_IsMutex(h);
        if (mtx && ms != 0) LD_BeforeAcquire(t, h, LD_MUTEX, "WaitForSingleObjectEx");
        if (!mtx) { LD_End(t); t = NULL; }
    }
    SetLastError(e0);
    DWORD r = RealWaitEx(h, ms, alertable);
    if (t) {
        DWORD e1 = GetLastError();
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) LD_AfterAcquire(t, h, LD_MUTEX, "WaitForSingleObjectEx", ms == 0);
        LD_End(t); SetLastError(e1);
    }
    return r;
}
static BOOL WINAPI HookReleaseMutex(HANDLE h) {
    void* ra = _ReturnAddress();
    DWORD e0 = GetLastError();
    LD_Thread* t = LD_Begin(ra);
    SetLastError(e0);
    BOOL r = RealReleaseMutex(h);
    if (t) { DWORD e1 = GetLastError(); if (r) LD_Release(t, h, LD_MUTEX, "ReleaseMutex"); LD_End(t); SetLastError(e1); }
    return r;
}
static BOOL WINAPI HookCloseHandle(HANDLE h) {
    void* ra = _ReturnAddress();
    DWORD e0 = GetLastError();
    LD_Thread* t = LD_Begin(ra);
    if (t) {
        if (LD_IsMutex(h)) { LD_UnregisterMutex(h); LD_ObjectDestroyed(h, LD_MUTEX); }
        LD_End(t);
    }
    SetLastError(e0);
    return RealCloseHandle(h);
}

// ---- SRW locks ----------------------------------------------------------------------------------
static VOID WINAPI HookAcquireEx(PSRWLOCK p) {
    void* ra = _ReturnAddress();
    DWORD e0 = GetLastError();
    LD_Thread* t = LD_Begin(ra);
    if (t) LD_BeforeAcquire(t, p, LD_SRW_EXCLUSIVE, "AcquireSRWLockExclusive");
    SetLastError(e0);
    RealAcquireEx(p);
    if (t) { DWORD e1 = GetLastError(); LD_AfterAcquire(t, p, LD_SRW_EXCLUSIVE, "AcquireSRWLockExclusive", false); LD_End(t); SetLastError(e1); }
}
static VOID WINAPI HookReleaseEx(PSRWLOCK p) {
    void* ra = _ReturnAddress();
    DWORD e0 = GetLastError();
    LD_Thread* t = LD_Begin(ra);
    if (t) { LD_Release(t, p, LD_SRW_EXCLUSIVE, "ReleaseSRWLockExclusive"); LD_End(t); }
    SetLastError(e0);
    RealReleaseEx(p);
}
static VOID WINAPI HookAcquireSh(PSRWLOCK p) {
    void* ra = _ReturnAddress();
    DWORD e0 = GetLastError();
    LD_Thread* t = LD_Begin(ra);
    if (t) LD_BeforeAcquire(t, p, LD_SRW_SHARED, "AcquireSRWLockShared");
    SetLastError(e0);
    RealAcquireSh(p);
    if (t) { DWORD e1 = GetLastError(); LD_AfterAcquire(t, p, LD_SRW_SHARED, "AcquireSRWLockShared", false); LD_End(t); SetLastError(e1); }
}
static VOID WINAPI HookReleaseSh(PSRWLOCK p) {
    void* ra = _ReturnAddress();
    DWORD e0 = GetLastError();
    LD_Thread* t = LD_Begin(ra);
    if (t) { LD_Release(t, p, LD_SRW_SHARED, "ReleaseSRWLockShared"); LD_End(t); }
    SetLastError(e0);
    RealReleaseSh(p);
}
static BOOLEAN WINAPI HookTryEx(PSRWLOCK p) {
    void* ra = _ReturnAddress();
    DWORD e0 = GetLastError();
    LD_Thread* t = LD_Begin(ra);
    SetLastError(e0);
    BOOLEAN r = RealTryEx(p);
    if (t) { DWORD e1 = GetLastError(); if (r) LD_AfterAcquire(t, p, LD_SRW_EXCLUSIVE, "TryAcquireSRWLockExclusive", true); LD_End(t); SetLastError(e1); }
    return r;
}
static BOOLEAN WINAPI HookTrySh(PSRWLOCK p) {
    void* ra = _ReturnAddress();
    DWORD e0 = GetLastError();
    LD_Thread* t = LD_Begin(ra);
    SetLastError(e0);
    BOOLEAN r = RealTrySh(p);
    if (t) { DWORD e1 = GetLastError(); if (r) LD_AfterAcquire(t, p, LD_SRW_SHARED, "TryAcquireSRWLockShared", true); LD_End(t); SetLastError(e1); }
    return r;
}


// ---- STL mutex in msvcp140.dll (_Mtx_*) ---------------------------------------------------
// With the dynamic CRT (/MD) std::mutex::lock() is a call into msvcp140.dll, which then calls
// AcquireSRWLockExclusive from inside that system DLL, so the caller filter ignores it.
// Hooking the exported _Mtx_* entry points instead sees the call coming from application code.
// The lock identity is the _Mtx_t argument (it points into the std::mutex object).
typedef int (__cdecl *PFN_MtxLock)(void*);
typedef int (__cdecl *PFN_MtxTimedLock)(void*, const void*);
typedef void (__cdecl *PFN_MtxDestroy)(void*);

template<int M> struct StlMtx {
    static PFN_MtxLock RealLock, RealTry, RealUnlock;
    static PFN_MtxTimedLock RealTimed;
    static PFN_MtxDestroy RealDestroy;

    static int __cdecl Lock(void* m) {
        void* ra = _ReturnAddress();
        DWORD e0 = GetLastError();
        LD_Thread* t = LD_Begin(ra);
        if (t) LD_BeforeAcquire(t, m, LD_STL_WRAPPER, "_Mtx_lock");
        SetLastError(e0);
        int r = RealLock(m);
        if (t) { DWORD e1 = GetLastError(); if (r == 0) LD_AfterAcquire(t, m, LD_STL_WRAPPER, "_Mtx_lock", false); LD_End(t); SetLastError(e1); }
        return r;
    }
    static int __cdecl Try(void* m) {
        void* ra = _ReturnAddress();
        DWORD e0 = GetLastError();
        LD_Thread* t = LD_Begin(ra);
        SetLastError(e0);
        int r = RealTry(m);
        if (t) { DWORD e1 = GetLastError(); if (r == 0) LD_AfterAcquire(t, m, LD_STL_WRAPPER, "_Mtx_trylock", true); LD_End(t); SetLastError(e1); }
        return r;
    }
    static int __cdecl Timed(void* m, const void* abs) {
        void* ra = _ReturnAddress();
        DWORD e0 = GetLastError();
        LD_Thread* t = LD_Begin(ra);
        if (t) LD_BeforeAcquire(t, m, LD_STL_WRAPPER, "_Mtx_timedlock");
        SetLastError(e0);
        int r = RealTimed(m, abs);
        if (t) { DWORD e1 = GetLastError(); if (r == 0) LD_AfterAcquire(t, m, LD_STL_WRAPPER, "_Mtx_timedlock", false); LD_End(t); SetLastError(e1); }
        return r;
    }
    static int __cdecl Unlock(void* m) {
        void* ra = _ReturnAddress();
        DWORD e0 = GetLastError();
        LD_Thread* t = LD_Begin(ra);
        if (t) { LD_Release(t, m, LD_STL_WRAPPER, "_Mtx_unlock"); LD_End(t); }
        SetLastError(e0);
        return RealUnlock(m);
    }
    static void __cdecl Destroy(void* m) {
        void* ra = _ReturnAddress();
        DWORD e0 = GetLastError();
        LD_Thread* t = LD_Begin(ra);
        if (t) { LD_ObjectDestroyed(m, LD_STL_WRAPPER); LD_End(t); }
        SetLastError(e0);
        RealDestroy(m);
    }
    // returns how many of the 5 entry points were attached
    static int Attach(HMODULE h) {
        RealLock    = (PFN_MtxLock)GetProcAddress(h, "_Mtx_lock");
        RealTry     = (PFN_MtxLock)GetProcAddress(h, "_Mtx_trylock");
        RealTimed   = (PFN_MtxTimedLock)GetProcAddress(h, "_Mtx_timedlock");
        RealUnlock  = (PFN_MtxLock)GetProcAddress(h, "_Mtx_unlock");
        RealDestroy = (PFN_MtxDestroy)GetProcAddress(h, "_Mtx_destroy_in_situ");
        int n = 0;
        if (RealLock)    { DetourAttach(&(PVOID&)RealLock, Lock); ++n; }
        if (RealTry)     { DetourAttach(&(PVOID&)RealTry, Try); ++n; }
        if (RealTimed)   { DetourAttach(&(PVOID&)RealTimed, Timed); ++n; }
        if (RealUnlock)  { DetourAttach(&(PVOID&)RealUnlock, Unlock); ++n; }
        if (RealDestroy) { DetourAttach(&(PVOID&)RealDestroy, Destroy); ++n; }
        return n;
    }
    static void Detach() {
        if (RealLock)    DetourDetach(&(PVOID&)RealLock, Lock);
        if (RealTry)     DetourDetach(&(PVOID&)RealTry, Try);
        if (RealTimed)   DetourDetach(&(PVOID&)RealTimed, Timed);
        if (RealUnlock)  DetourDetach(&(PVOID&)RealUnlock, Unlock);
        if (RealDestroy) DetourDetach(&(PVOID&)RealDestroy, Destroy);
    }
};
template<int M> PFN_MtxLock StlMtx<M>::RealLock = NULL;
template<int M> PFN_MtxLock StlMtx<M>::RealTry = NULL;
template<int M> PFN_MtxLock StlMtx<M>::RealUnlock = NULL;
template<int M> PFN_MtxTimedLock StlMtx<M>::RealTimed = NULL;
template<int M> PFN_MtxDestroy StlMtx<M>::RealDestroy = NULL;

// ---- install / remove -----------------------------------------------------------------------------
// Detours only fixes up threads it was told about. Register every thread of the process so
// none of them can be executing inside a function while its first bytes are rewritten.
static int g_stlAttached[2] = { 0, 0 };
static HANDLE g_updated[1024];
static int g_updatedCount = 0;
static void UpdateAllThreads() {
    g_updatedCount = 0;
    DetourUpdateThread(GetCurrentThread());
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            if (g_updatedCount >= 1024) break;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
            if (!h) continue;
            if (DetourUpdateThread(h) == NO_ERROR) g_updated[g_updatedCount++] = h;
            else CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}
static void ReleaseUpdatedThreads() {
    for (int i = 0; i < g_updatedCount; ++i) CloseHandle(g_updated[i]);
    g_updatedCount = 0;
}

BOOL LD_InstallHooks() {
    // only DLLs that are already loaded can be hooked; msvcp140d.dll is the debug CRT
    HMODULE stl0 = GetModuleHandleW(L"msvcp140.dll");
    HMODULE stl1 = GetModuleHandleW(L"msvcp140d.dll");
    int n0 = 0, n1 = 0;
    DetourTransactionBegin();
    UpdateAllThreads();
    DetourAttach(&(PVOID&)RealEnterCS, HookEnterCS);
    DetourAttach(&(PVOID&)RealTryEnterCS, HookTryEnterCS);
    DetourAttach(&(PVOID&)RealLeaveCS, HookLeaveCS);
    DetourAttach(&(PVOID&)RealDeleteCS, HookDeleteCS);
    DetourAttach(&(PVOID&)RealCreateMutexW, HookCreateMutexW);
    DetourAttach(&(PVOID&)RealCreateMutexA, HookCreateMutexA);
    DetourAttach(&(PVOID&)RealWait, HookWait);
    DetourAttach(&(PVOID&)RealWaitEx, HookWaitEx);
    DetourAttach(&(PVOID&)RealReleaseMutex, HookReleaseMutex);
    DetourAttach(&(PVOID&)RealCloseHandle, HookCloseHandle);
    DetourAttach(&(PVOID&)RealAcquireEx, HookAcquireEx);
    DetourAttach(&(PVOID&)RealReleaseEx, HookReleaseEx);
    DetourAttach(&(PVOID&)RealAcquireSh, HookAcquireSh);
    DetourAttach(&(PVOID&)RealReleaseSh, HookReleaseSh);
    DetourAttach(&(PVOID&)RealTryEx, HookTryEx);
    DetourAttach(&(PVOID&)RealTrySh, HookTrySh);
    if (stl0) n0 = StlMtx<0>::Attach(stl0);
    if (stl1) n1 = StlMtx<1>::Attach(stl1);
    LONG rc = DetourTransactionCommit();
    ReleaseUpdatedThreads();
    g_stlAttached[0] = (rc == NO_ERROR) ? n0 : 0;
    g_stlAttached[1] = (rc == NO_ERROR) ? n1 : 0;
    if (rc == NO_ERROR) {
        LD_LogMessage("INFO", stl0 ? (n0 == 5 ? "STL std::mutex hooks active (msvcp140.dll _Mtx_*: 5/5)"
                                              : "STL std::mutex hooks PARTIAL (msvcp140.dll _Mtx_* missing exports)")
                                   : "msvcp140.dll not loaded: only statically linked std::mutex is tracked");
        if (stl1) LD_LogMessage("INFO", "msvcp140d.dll (debug CRT) std::mutex hooks attached");
    }
    return rc == NO_ERROR;
}

void LD_RemoveHooks() {
    DetourTransactionBegin();
    UpdateAllThreads();
    DetourDetach(&(PVOID&)RealEnterCS, HookEnterCS);
    DetourDetach(&(PVOID&)RealTryEnterCS, HookTryEnterCS);
    DetourDetach(&(PVOID&)RealLeaveCS, HookLeaveCS);
    DetourDetach(&(PVOID&)RealDeleteCS, HookDeleteCS);
    DetourDetach(&(PVOID&)RealCreateMutexW, HookCreateMutexW);
    DetourDetach(&(PVOID&)RealCreateMutexA, HookCreateMutexA);
    DetourDetach(&(PVOID&)RealWait, HookWait);
    DetourDetach(&(PVOID&)RealWaitEx, HookWaitEx);
    DetourDetach(&(PVOID&)RealReleaseMutex, HookReleaseMutex);
    DetourDetach(&(PVOID&)RealCloseHandle, HookCloseHandle);
    DetourDetach(&(PVOID&)RealAcquireEx, HookAcquireEx);
    DetourDetach(&(PVOID&)RealReleaseEx, HookReleaseEx);
    DetourDetach(&(PVOID&)RealAcquireSh, HookAcquireSh);
    DetourDetach(&(PVOID&)RealReleaseSh, HookReleaseSh);
    DetourDetach(&(PVOID&)RealTryEx, HookTryEx);
    DetourDetach(&(PVOID&)RealTrySh, HookTrySh);
    if (g_stlAttached[0]) StlMtx<0>::Detach();
    if (g_stlAttached[1]) StlMtx<1>::Detach();
    DetourTransactionCommit();
    ReleaseUpdatedThreads();
}
