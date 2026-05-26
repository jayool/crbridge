#include "host_alias.h"
#include <detours.h>
#include <cstdio>
#include <cstring>

namespace {

    void LogLine(const char* msg) {
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

    // The module name the ost-branch cloud_redirect.dll looks for.
    HMODULE g_diverted = nullptr;

    using GetModuleHandleA_t = HMODULE (WINAPI*)(LPCSTR);
    using GetModuleHandleW_t = HMODULE (WINAPI*)(LPCWSTR);
    GetModuleHandleA_t g_origA = GetModuleHandleA;
    GetModuleHandleW_t g_origW = GetModuleHandleW;

    HMODULE WINAPI HookA(LPCSTR name) {
        if (g_diverted && name && _stricmp(name, "diversion.dll") == 0)
            return g_diverted;
        return g_origA(name);
    }
    HMODULE WINAPI HookW(LPCWSTR name) {
        if (g_diverted && name && _wcsicmp(name, L"diversion.dll") == 0)
            return g_diverted;
        return g_origW(name);
    }
}

namespace HostAlias {

bool Install(HMODULE diverted) {
    if (!diverted) {
        LogLine("HostAlias: diverted module is null, aborting");
        return false;
    }
    g_diverted = diverted;

    if (DetourTransactionBegin() != NO_ERROR) {
        LogLine("HostAlias: DetourTransactionBegin failed");
        return false;
    }
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&g_origA), reinterpret_cast<PVOID>(&HookA));
    DetourAttach(reinterpret_cast<PVOID*>(&g_origW), reinterpret_cast<PVOID>(&HookW));
    if (DetourTransactionCommit() != NO_ERROR) {
        LogLine("HostAlias: DetourTransactionCommit failed");
        return false;
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
        "HostAlias: \"diversion.dll\" aliased to diverted module @ %p", (void*)diverted);
    LogLine(buf);
    return true;
}

} // namespace HostAlias
