#include "cr_loader.h"
#include <cstdio>
#include <string>

namespace {
    HMODULE g_crModule = nullptr;
    CRLoader::CR_InitCloudSave_t g_initCloudSave = nullptr;
    CRLoader::CR_SetApps_t       g_setApps       = nullptr;
    CRLoader::CR_Shutdown_t      g_shutdown      = nullptr;

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

    std::string GetHostDir() {
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string path(exePath);
        size_t pos = path.find_last_of("\\/");
        return (pos != std::string::npos) ? path.substr(0, pos) : "";
    }
}

namespace CRLoader {

bool TryLoad() {
    if (g_crModule) return true;

    // Steam loads its DLLs from the install dir, so try there first. The
    // system search fallback covers configurations where cloud_redirect.dll
    // lives elsewhere on PATH (uncommon but harmless to support).
    std::string hostDir = GetHostDir();
    std::string crPath = hostDir + "\\cloud_redirect.dll";

    g_crModule = LoadLibraryA(crPath.c_str());
    if (!g_crModule) {
        char buf[640];
        snprintf(buf, sizeof(buf),
            "CRLoader: LoadLibrary failed for '%s' (error %lu); trying system search path",
            crPath.c_str(), GetLastError());
        LogLine(buf);
        g_crModule = LoadLibraryA("cloud_redirect.dll");
        if (!g_crModule) {
            snprintf(buf, sizeof(buf),
                "CRLoader: fallback LoadLibrary('cloud_redirect.dll') also failed (error %lu). "
                "Make sure cloud_redirect.dll is next to steam.exe.",
                GetLastError());
            LogLine(buf);
            return false;
        }
    }

    char buf[640];
    snprintf(buf, sizeof(buf),
        "CRLoader: cloud_redirect.dll loaded at %p", (void*)g_crModule);
    LogLine(buf);

    g_initCloudSave = (CR_InitCloudSave_t)GetProcAddress(g_crModule, "CR_InitCloudSave");
    g_setApps       = (CR_SetApps_t)      GetProcAddress(g_crModule, "CR_SetApps");
    g_shutdown      = (CR_Shutdown_t)     GetProcAddress(g_crModule, "CR_Shutdown");

    // CR_InitCloudSave is the only hard requirement. Without it we can't
    // boot CR at all. The other two are best-effort: SetApps is only
    // useful if we ever want to feed CR a dynamic namespace-app list from
    // outside its config.json, and Shutdown just lets us drain cleanly
    // on DLL detach.
    if (!g_initCloudSave) {
        LogLine("CRLoader: CR_InitCloudSave export not found — is this cloud_redirect.dll "
                "v2.1.6 or newer? Older builds don't have this API.");
        return false;
    }
    if (!g_setApps) {
        LogLine("CRLoader: CR_SetApps export not found (continuing — CR will use its config.json for the app list)");
    }
    if (!g_shutdown) {
        LogLine("CRLoader: CR_Shutdown export not found (continuing — DLL_PROCESS_DETACH will skip the clean-shutdown call)");
    }

    snprintf(buf, sizeof(buf),
        "CRLoader: resolved API: CR_InitCloudSave=%p CR_SetApps=%p CR_Shutdown=%p",
        (void*)g_initCloudSave, (void*)g_setApps, (void*)g_shutdown);
    LogLine(buf);
    return true;
}

CR_InitCloudSave_t GetInitCloudSave() { return g_initCloudSave; }
CR_SetApps_t       GetSetApps()       { return g_setApps; }
CR_Shutdown_t      GetShutdown()      { return g_shutdown; }
HMODULE            GetModule()        { return g_crModule; }

}  // namespace CRLoader
