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

    FILE* f = nullptr;
    fopen_s(&f, "C:\\crbridge.log", "a");
    if (!f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] crbridge loaded into PID %lu, host=%s\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond,
        GetCurrentProcessId(), exePath);
    fclose(f);
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
