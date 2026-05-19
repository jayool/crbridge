// crbridge — standalone loader for CloudRedirect
// Iteration 1: prove the DLL gets loaded into a host process.
//
// On load, append a line to C:\crbridge.log with the host process path
// and PID. If we see that line after injecting into Steam, basic loading
// works and we can move to Iteration 2 (load cloud_redirect.dll, resolve
// CloudOnSendPkt).

#include <windows.h>
#include <cstdio>

static void LogLoad() {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);

    char tempDir[MAX_PATH] = {};
    if (GetTempPathA(MAX_PATH, tempDir) == 0) return;

    char logPath[MAX_PATH] = {};
    snprintf(logPath, MAX_PATH, "%scrbridge.log", tempDir);

    FILE* f = nullptr;
    fopen_s(&f, logPath, "a");
    if (!f) {
        // Si incluso TEMP falla, al menos manda al debugger
        OutputDebugStringA("crbridge: failed to open log file\n");
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] crbridge loaded into PID %lu, host=%s\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond,
        GetCurrentProcessId(), exePath);
    fclose(f);
    
    // También mandamos al debugger por si quieres mirarlo con DebugView
    char dbgMsg[1024];
    snprintf(dbgMsg, sizeof(dbgMsg), "crbridge: loaded into PID %lu, log=%s\n",
             GetCurrentProcessId(), logPath);
    OutputDebugStringA(dbgMsg);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        LogLoad();
        break;
    }
    return TRUE;
}
