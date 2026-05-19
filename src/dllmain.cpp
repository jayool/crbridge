// crbridge — standalone loader for CloudRedirect
// Iteration 2: load cloud_redirect.dll and resolve CloudOnSendPkt.
// All real work is done in InitThread (outside loader lock) to allow
// LoadLibrary calls without deadlock risk.

#include <windows.h>
#include <cstdio>
#include "cr_loader.h"

static void LogLoad() {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);

    char tempDir[MAX_PATH] = {};
    if (GetTempPathA(MAX_PATH, tempDir) == 0) return;

    char logPath[MAX_PATH] = {};
    snprintf(logPath, MAX_PATH, "%scrbridge.log", tempDir);

    FILE* f = nullptr;
    fopen_s(&f, logPath, "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] crbridge loaded into PID %lu, host=%s\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond,
            GetCurrentProcessId(), exePath);
        fclose(f);
    }

    char dbgMsg[1024];
    snprintf(dbgMsg, sizeof(dbgMsg),
        "crbridge: loaded into PID %lu", GetCurrentProcessId());
    OutputDebugStringA(dbgMsg);
}

static DWORD WINAPI InitThread(LPVOID) {
    LogLoad();
    CRLoader::TryLoad();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        // Spawn thread so we can safely call LoadLibrary outside the loader lock.
        // DllMain MUST return quickly; doing LoadLibrary here can deadlock.
        {
            HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
            if (h) CloseHandle(h);
        }
        break;
    }
    return TRUE;
}
