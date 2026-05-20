// crbridge — Branch: experiment/cr-warmup
//
// Iteration 13 (experimental): trigger CR's lazy init via a warm-up call to
// CloudOnSendPkt BEFORE installing the diversion hook. Hypothesis: when our
// hook fires from the diversion, CR's RTTI walking for CCMInterface gets
// confused. By forcing init from our clean InitThread context (no diversion
// involvement), CR should resolve CCM correctly and its [INJECT] mechanism
// should work.

#include <windows.h>
#include <cstdio>
#include <climits>
#include "cr_loader.h"
#include "steam_locator.h"
#include "function_hook.h"

static void DllmainLogLine(const char* msg) {
    char tempDir[MAX_PATH] = {};
    if (GetTempPathA(MAX_PATH, tempDir) == 0) return;
    char logPath[MAX_PATH] = {};
    snprintf(logPath, MAX_PATH, "%scrbridge.log", tempDir);
    FILE* f = nullptr;
    fopen_s(&f, logPath, "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, msg);
        fclose(f);
    }
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

static void LogLoad() {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "crbridge loaded into PID %lu, host=%s",
        GetCurrentProcessId(), exePath);
    DllmainLogLine(buf);
}

// Wrapped in its own function because __try/__except has restrictions when
// mixed with C++ object construction. CR may crash on null packet; we want
// the init to run but the crash to be contained.
static int CallCloudOnSendPktSafe(CRLoader::CloudOnSendPkt_t fn) {
    __try {
        return fn(nullptr, nullptr, 0, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return INT_MIN;
    }
}

static void WarmUpCloudRedirect() {
    auto crFn = CRLoader::GetCloudOnSendPkt();
    if (!crFn) {
        DllmainLogLine("CR warm-up: skipped (CloudOnSendPkt not loaded)");
        return;
    }

    DllmainLogLine("CR warm-up: calling CloudOnSendPkt(null, null, 0, null) "
                   "from clean InitThread context to trigger lazy init");
    int result = CallCloudOnSendPktSafe(crFn);
    if (result == INT_MIN) {
        DllmainLogLine("CR warm-up: caught exception "
                       "(expected if CR rejects null packet; init should still have run)");
    } else {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "CR warm-up: returned %d cleanly (no exception)", result);
        DllmainLogLine(buf);
    }
}

static DWORD WINAPI InitThread(LPVOID) {
    LogLoad();
    CRLoader::TryLoad();

    // EXPERIMENT: warm up CR's lazy init before we install the diversion hook
    WarmUpCloudRedirect();

    SteamLocator::DiagnoseRTTI();

    const void* sendFrame = SteamLocator::FindBBuildAndAsyncSendFrame();
    if (sendFrame) {
        FunctionHook::InstallBBuildAndAsyncSendFrame(const_cast<void*>(sendFrame));
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        {
            HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
            if (h) CloseHandle(h);
        }
        break;
    }
    return TRUE;
}
