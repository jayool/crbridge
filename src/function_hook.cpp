#include "function_hook.h"
#include "cr_loader.h"
#include <MinHook.h>
#include <cstdio>
#include <cstdint>
#include <atomic>

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

    // BBuildAndAsyncSendFrame signature (matches LumaCore PacketRouter.cpp)
    using BBAASF_t = bool (__fastcall*)(void*, int, uint8_t*, uint32_t);
    BBAASF_t g_orig = nullptr;

    constexpr int kWebSocketOpCodeBinary = 2;

    std::atomic<int> g_totalCount{0};
    std::atomic<int> g_swallowedCount{0};
    constexpr int kMaxLogTotal = 5;
    constexpr int kMaxLogSwallowed = 20;

    bool __fastcall hkBBuildAndAsyncSendFrame(void* pObject, int opcode,
                                              uint8_t* pubData, uint32_t cubData) {
        int total = g_totalCount.fetch_add(1) + 1;

        // Log the first few outgoing frames for diagnostics
        if (total <= kMaxLogTotal) {
            char buf[320];
            snprintf(buf, sizeof(buf),
                "BBAASF #%d: pObject=%p opcode=%d size=%u",
                total, pObject, opcode, cubData);
            LogLine(buf);
        }

        // Only forward binary frames to CR (text/ping/pong frames don't carry RPCs)
        if (opcode == kWebSocketOpCodeBinary && pubData && cubData > 0) {
            auto crFn = CRLoader::GetCloudOnSendPkt();
            if (crFn) {
                int swallow = crFn(pObject, pubData, cubData, nullptr);
                if (swallow) {
                    int s = g_swallowedCount.fetch_add(1) + 1;
                    if (s <= kMaxLogSwallowed) {
                        char buf[200];
                        snprintf(buf, sizeof(buf),
                            "BBAASF #%d -> CR SWALLOWED (total swallowed: %d)", total, s);
                        LogLine(buf);
                    }
                    return true;  // tell Steam the frame was "sent" successfully
                }
            }
        }

        return g_orig(pObject, opcode, pubData, cubData);
    }
}

namespace FunctionHook {

bool InstallBBuildAndAsyncSendFrame(void* target) {
    if (!target) {
        LogLine("FunctionHook: target is null, aborting");
        return false;
    }

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "FunctionHook: MH_Initialize failed (status=%d)", (int)status);
        LogLine(buf);
        return false;
    }

    status = MH_CreateHook(target,
                           reinterpret_cast<void*>(&hkBBuildAndAsyncSendFrame),
                           reinterpret_cast<void**>(&g_orig));
    if (status != MH_OK) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "FunctionHook: MH_CreateHook failed (status=%d)", (int)status);
        LogLine(buf);
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "FunctionHook: MH_EnableHook failed (status=%d)", (int)status);
        LogLine(buf);
        return false;
    }

    char buf[200];
    snprintf(buf, sizeof(buf),
        "FunctionHook: BBuildAndAsyncSendFrame hooked at %p, CR forwarding ENABLED",
        target);
    LogLine(buf);
    return true;
}

} // namespace FunctionHook
