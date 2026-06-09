#include "steam_locator.h"
#include <cstdio>

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

}  // namespace

namespace SteamLocator {

HMODULE WaitForSteamClient(int maxMs) {
    constexpr int kStepMs = 100;
    for (int waited = 0; waited <= maxMs; waited += kStepMs) {
        HMODULE h = GetModuleHandleA("steamclient64.dll");
        if (h) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                "SteamLocator: steamclient64.dll loaded at %p after %dms",
                (void*)h, waited);
            LogLine(buf);
            return h;
        }
        Sleep(kStepMs);
    }
    char buf[120];
    snprintf(buf, sizeof(buf),
        "SteamLocator: steamclient64.dll not loaded after %dms", maxMs);
    LogLine(buf);
    return nullptr;
}

HMODULE WaitForDiversion(int maxMs, const char** outFoundName) {
    constexpr int kStepMs = 100;
    // LumaCore copies steamclient64.dll to lcoverlay.dll (modern) or
    // diversion.dll (legacy) and routes Steam's runtime through it. We
    // poll for both because the load order between our version.dll proxy
    // and LumaCore's dwmapi.dll proxy is racy.
    const char* candidates[] = { "lcoverlay.dll", "diversion.dll" };
    for (int waited = 0; waited <= maxMs; waited += kStepMs) {
        for (const char* name : candidates) {
            HMODULE h = GetModuleHandleA(name);
            if (h) {
                if (outFoundName) *outFoundName = name;
                char buf[200];
                snprintf(buf, sizeof(buf),
                    "SteamLocator: diversion module %s loaded at %p after %dms",
                    name, (void*)h, waited);
                LogLine(buf);
                return h;
            }
        }
        Sleep(kStepMs);
    }
    char buf[200];
    snprintf(buf, sizeof(buf),
        "SteamLocator: no diversion module (lcoverlay.dll/diversion.dll) "
        "after %dms — proceeding without LumaCore redirection",
        maxMs);
    LogLine(buf);
    return nullptr;
}

}  // namespace SteamLocator
