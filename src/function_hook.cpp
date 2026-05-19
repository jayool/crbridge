#include "function_hook.h"
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

    // Signature matches LumaCore PacketRouter.cpp:
    //   bool BBuildAndAsyncSendFrame(void* pObject, int eWebSocketOpCode,
    //                                 uint8_t* pubData, uint32_t cubData)
    using BBAASF_t = bool (__fastcall*)(void*, int, uint8_t*, uint32_t);
    BBAASF_t g_orig = nullptr;
    std::atomic<int> g_hitCount{0};
    constexpr int kMaxLog = 10;

    bool __fastcall hkBBuildAndAsyncSendFrame(void* pObject, int opcode,
                                              uint8_t* pubData, uint32_t cubData) {
        int count = g_hitCount.fetch_add(1) + 1;
        if (count <= kMaxLog) {
            char buf[320];
            snprintf(buf, sizeof(buf),
                "BBAASF HIT #%d: pObject=%p opcode=%d pubData=%p size=%u",
                count, pObject, opcode, (void*)pubData, cubData);
            LogLine(buf);

            // Dump first 48 bytes of pubData (the actual packet)
            if (pubData && cubData > 0) {
                size_t bytes = (cubData < 48) ? cubData : 48;
                char hex[256] = {};
                char ascii[64] = {};
                for (size_t i = 0; i < bytes; ++i) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "%02X ", pubData[i]);
                    strncat_s(hex, sizeof(hex), tmp, _TRUNCATE);
                    char c = (pubData[i] >= 32 && pubData[i] < 127)
                        ? static_cast<char>(pubData[i]) : '.';
                    char ctmp[2] = { c, 0 };
                    strncat_s(ascii, sizeof(ascii), ctmp, _TRUNCATE);
                }
                char buf2[640];
                snprintf(buf2, sizeof(buf2), "    pubData: %s | %s", hex, ascii);
                LogLine(buf2);
            }
        }

        // Forward to original
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

    char buf[160];
    snprintf(buf, sizeof(buf),
        "FunctionHook: BBuildAndAsyncSendFrame hooked at %p", target);
    LogLine(buf);
    return true;
}

} // namespace FunctionHook
