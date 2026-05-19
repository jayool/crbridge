#include "cr_loader.h"
#include <cstdio>
#include <string>

namespace {
    HMODULE g_crModule = nullptr;
    CRLoader::CloudOnSendPkt_t g_pCloudOnSendPkt = nullptr;

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

    std::string hostDir = GetHostDir();
    std::string crPath = hostDir + "\\cloud_redirect.dll";

    g_crModule = LoadLibraryA(crPath.c_str());
    if (!g_crModule) {
        char buf[640];
        snprintf(buf, sizeof(buf),
            "CRLoader: LoadLibrary failed for '%s' (error %lu)",
            crPath.c_str(), GetLastError());
        LogLine(buf);
        // Fallback: try system search path
        g_crModule = LoadLibraryA("cloud_redirect.dll");
        if (!g_crModule) {
            snprintf(buf, sizeof(buf),
                "CRLoader: fallback LoadLibrary('cloud_redirect.dll') also failed (error %lu)",
                GetLastError());
            LogLine(buf);
            return false;
        }
    }

    char buf[640];
    snprintf(buf, sizeof(buf),
        "CRLoader: cloud_redirect.dll loaded at %p", (void*)g_crModule);
    LogLine(buf);

    g_pCloudOnSendPkt = (CloudOnSendPkt_t)GetProcAddress(g_crModule, "CloudOnSendPkt");
    if (!g_pCloudOnSendPkt) {
        snprintf(buf, sizeof(buf),
            "CRLoader: GetProcAddress(CloudOnSendPkt) failed (error %lu)",
            GetLastError());
        LogLine(buf);
        return false;
    }

    snprintf(buf, sizeof(buf),
        "CRLoader: CloudOnSendPkt resolved at %p", (void*)g_pCloudOnSendPkt);
    LogLine(buf);
    return true;
}

CloudOnSendPkt_t GetCloudOnSendPkt() {
    return g_pCloudOnSendPkt;
}

} // namespace CRLoader
