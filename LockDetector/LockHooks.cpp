#include "LockHooks.h"
#include "LockTracker.h"
#include <detours.h>

typedef VOID (WINAPI *PFN_EnterCriticalSection)(LPCRITICAL_SECTION);
typedef BOOL (WINAPI *PFN_TryEnterCriticalSection)(LPCRITICAL_SECTION);
typedef VOID (WINAPI *PFN_LeaveCriticalSection)(LPCRITICAL_SECTION);
typedef HANDLE (WINAPI *PFN_CreateMutexW)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
typedef DWORD (WINAPI *PFN_WaitForSingleObject)(HANDLE, DWORD);
typedef BOOL (WINAPI *PFN_ReleaseMutex)(HANDLE);
typedef VOID (WINAPI *PFN_AcquireSRWLockExclusive)(PSRWLOCK);
typedef VOID (WINAPI *PFN_ReleaseSRWLockExclusive)(PSRWLOCK);
typedef VOID (WINAPI *PFN_AcquireSRWLockShared)(PSRWLOCK);
typedef VOID (WINAPI *PFN_ReleaseSRWLockShared)(PSRWLOCK);
typedef BOOLEAN (WINAPI *PFN_TryAcquireSRWLockExclusive)(PSRWLOCK);
typedef BOOLEAN (WINAPI *PFN_TryAcquireSRWLockShared)(PSRWLOCK);

static PFN_EnterCriticalSection RealEnterCS = EnterCriticalSection;
static PFN_TryEnterCriticalSection RealTryEnterCS = TryEnterCriticalSection;
static PFN_LeaveCriticalSection RealLeaveCS = LeaveCriticalSection;
static PFN_CreateMutexW RealCreateMutexW = CreateMutexW;
static PFN_WaitForSingleObject RealWait = WaitForSingleObject;
static PFN_ReleaseMutex RealReleaseMutex = ReleaseMutex;
static PFN_AcquireSRWLockExclusive RealAcquireEx = AcquireSRWLockExclusive;
static PFN_ReleaseSRWLockExclusive RealReleaseEx = ReleaseSRWLockExclusive;
static PFN_AcquireSRWLockShared RealAcquireSh = AcquireSRWLockShared;
static PFN_ReleaseSRWLockShared RealReleaseSh = ReleaseSRWLockShared;
static PFN_TryAcquireSRWLockExclusive RealTryEx = TryAcquireSRWLockExclusive;
static PFN_TryAcquireSRWLockShared RealTrySh = TryAcquireSRWLockShared;

#include <set>
#include <map>
static CRITICAL_SECTION g_handleGuard;
static bool g_handleGuardReady = false;
static std::set<HANDLE> g_mutexHandles;

static VOID WINAPI HookEnterCS(LPCRITICAL_SECTION p) {
    RealEnterCS(p);
    LD_TrackAcquire(p, LD_CRITICAL_SECTION, "EnterCriticalSection");
}
static BOOL WINAPI HookTryEnterCS(LPCRITICAL_SECTION p) {
    BOOL ok = RealTryEnterCS(p);
    if (ok) LD_TrackAcquire(p, LD_CRITICAL_SECTION, "TryEnterCriticalSection");
    return ok;
}
static VOID WINAPI HookLeaveCS(LPCRITICAL_SECTION p) {
    LD_TrackRelease(p, LD_CRITICAL_SECTION, "LeaveCriticalSection");
    RealLeaveCS(p);
}
static HANDLE WINAPI HookCreateMutexW(LPSECURITY_ATTRIBUTES a, BOOL owner, LPCWSTR name) {
    HANDLE h = RealCreateMutexW(a, owner, name);
    if (h && h != INVALID_HANDLE_VALUE) {
        EnterCriticalSection(&g_handleGuard); g_mutexHandles.insert(h); LeaveCriticalSection(&g_handleGuard);
        if (owner) LD_TrackAcquire(h, LD_MUTEX, "CreateMutexW(initial owner)");
    }
    return h;
}
static DWORD WINAPI HookWait(HANDLE h, DWORD ms) {
    DWORD r = RealWait(h, ms);
    bool isMutex = false;
    EnterCriticalSection(&g_handleGuard);
    isMutex = (g_mutexHandles.find(h) != g_mutexHandles.end());
    LeaveCriticalSection(&g_handleGuard);
    if (isMutex && (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED))
        LD_TrackAcquire(h, LD_MUTEX, "WaitForSingleObject");
    return r;
}
static BOOL WINAPI HookReleaseMutex(HANDLE h) {
    BOOL r = RealReleaseMutex(h);
    if (r) LD_TrackRelease(h, LD_MUTEX, "ReleaseMutex");
    return r;
}
static VOID WINAPI HookAcquireEx(PSRWLOCK p) {
    RealAcquireEx(p); LD_TrackAcquire(p, LD_SRW_EXCLUSIVE, "AcquireSRWLockExclusive");
}
static VOID WINAPI HookReleaseEx(PSRWLOCK p) {
    LD_TrackRelease(p, LD_SRW_EXCLUSIVE, "ReleaseSRWLockExclusive"); RealReleaseEx(p);
}
static VOID WINAPI HookAcquireSh(PSRWLOCK p) {
    RealAcquireSh(p); LD_TrackAcquire(p, LD_SRW_SHARED, "AcquireSRWLockShared");
}
static VOID WINAPI HookReleaseSh(PSRWLOCK p) {
    LD_TrackRelease(p, LD_SRW_SHARED, "ReleaseSRWLockShared"); RealReleaseSh(p);
}
static BOOLEAN WINAPI HookTryEx(PSRWLOCK p) {
    BOOLEAN r = RealTryEx(p); if (r) LD_TrackAcquire(p, LD_SRW_EXCLUSIVE, "TryAcquireSRWLockExclusive"); return r;
}
static BOOLEAN WINAPI HookTrySh(PSRWLOCK p) {
    BOOLEAN r = RealTrySh(p); if (r) LD_TrackAcquire(p, LD_SRW_SHARED, "TryAcquireSRWLockShared"); return r;
}

BOOL LD_InstallHooks() {
    InitializeCriticalSection(&g_handleGuard); g_handleGuardReady = true;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)RealEnterCS, HookEnterCS);
    DetourAttach(&(PVOID&)RealTryEnterCS, HookTryEnterCS);
    DetourAttach(&(PVOID&)RealLeaveCS, HookLeaveCS);
    DetourAttach(&(PVOID&)RealCreateMutexW, HookCreateMutexW);
    DetourAttach(&(PVOID&)RealWait, HookWait);
    DetourAttach(&(PVOID&)RealReleaseMutex, HookReleaseMutex);
    DetourAttach(&(PVOID&)RealAcquireEx, HookAcquireEx);
    DetourAttach(&(PVOID&)RealReleaseEx, HookReleaseEx);
    DetourAttach(&(PVOID&)RealAcquireSh, HookAcquireSh);
    DetourAttach(&(PVOID&)RealReleaseSh, HookReleaseSh);
    DetourAttach(&(PVOID&)RealTryEx, HookTryEx);
    DetourAttach(&(PVOID&)RealTrySh, HookTrySh);
    return DetourTransactionCommit() == NO_ERROR;
}
void LD_RemoveHooks() {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)RealEnterCS, HookEnterCS);
    DetourDetach(&(PVOID&)RealTryEnterCS, HookTryEnterCS);
    DetourDetach(&(PVOID&)RealLeaveCS, HookLeaveCS);
    DetourDetach(&(PVOID&)RealCreateMutexW, HookCreateMutexW);
    DetourDetach(&(PVOID&)RealWait, HookWait);
    DetourDetach(&(PVOID&)RealReleaseMutex, HookReleaseMutex);
    DetourDetach(&(PVOID&)RealAcquireEx, HookAcquireEx);
    DetourDetach(&(PVOID&)RealReleaseEx, HookReleaseEx);
    DetourDetach(&(PVOID&)RealAcquireSh, HookAcquireSh);
    DetourDetach(&(PVOID&)RealReleaseSh, HookReleaseSh);
    DetourDetach(&(PVOID&)RealTryEx, HookTryEx);
    DetourDetach(&(PVOID&)RealTrySh, HookTrySh);
    DetourTransactionCommit();
}
