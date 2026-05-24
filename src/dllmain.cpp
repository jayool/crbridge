// crbridge — standalone loader for CloudRedirect
// Iteration 8: hook BBuildAndAsyncSendFrame via MinHook and observe packets.

#include <windows.h>
#include <cstdio>
#include "cr_loader.h"
#include "steam_locator.h"
#include "function_hook.h"
#include "version_check.h"

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
    // Report Steam-vs-CR compatibility right after CR is in memory, before
    // any heavy hook work. This way the verdict reaches the log even if a
    // later step fails or the user only ever looks at the first few lines.
    VersionCheck::Run();
    SteamLocator::DiagnoseRTTI();

    // Iteration 8: actually hook BBuildAndAsyncSendFrame
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
