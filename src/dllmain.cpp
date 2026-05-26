// crbridge — minimal loader that lets an ost-branch CloudRedirect run under
// SteaMidra/LumaCore. Loaded by the version.dll proxy inside steam.exe.

#include <windows.h>
#include <cstdio>
#include "cr_loader.h"
#include "host_alias.h"
#include "version_check.h"

static void LogLine(const char* msg) {
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

// SteaMidra/LumaCore copies steamclient64.dll to bin\lcoverlay.dll (older
// builds used diversion.dll) and routes Steam's CM traffic through that copy.
static HMODULE WaitForDivertedModule(int maxMs) {
    const char* candidates[] = { "lcoverlay.dll", "diversion.dll" };
    const int step = 100;
    for (int waited = 0; waited <= maxMs; waited += step) {
        for (const char* name : candidates) {
            HMODULE h = GetModuleHandleA(name);
            if (h) return h;
        }
        Sleep(step);
    }
    return nullptr;
}

static DWORD WINAPI InitThread(LPVOID) {
    char buf[160];
    snprintf(buf, sizeof(buf), "crbridge loaded into PID %lu", GetCurrentProcessId());
    LogLine(buf);

    HMODULE diverted = WaitForDivertedModule(15000);
    if (!diverted) {
        LogLine("crbridge: lcoverlay.dll/diversion.dll not found after 15s. "
                "Is SteaMidra installed and running? CR not loaded.");
        return 0;
    }

    // Alias the name CR expects BEFORE loading it: CR's self-init thread reads
    // GetModuleHandleA("diversion.dll") from inside its own DllMain.
    HostAlias::Install(diverted);

    if (!CRLoader::TryLoad()) {
        LogLine("crbridge: cloud_redirect.dll not loaded; nothing to bridge.");
        return 0;
    }

    // Diagnostics only: log whether this Steam build is in CR's whitelist.
    VersionCheck::Run();

    LogLine("crbridge: setup complete; CloudRedirect is self-initializing.");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
