#include "function_hook.h"
#include "cr_loader.h"
#include <detours.h>
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
        if (total <= kMaxLogTotal) {
            char buf[320];
            snprintf(buf, sizeof(buf),
                "BBAASF #%d: pObject=%p opcode=%d size=%u",
                total, pObject, opcode, cubData);
            LogLine(buf);
        }

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
                    return true;
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

    // Detours requires g_orig to point at the target BEFORE DetourAttach.
    // The Attach call updates g_orig to point at the trampoline (which may
    // chain through any existing Detours hook installed by LumaCore).
    g_orig = reinterpret_cast<BBAASF_t>(target);

    LONG err = DetourTransactionBegin();
    if (err != NO_ERROR) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "FunctionHook: DetourTransactionBegin failed (err=%ld)", err);
        LogLine(buf);
        return false;
    }

    err = DetourUpdateThread(GetCurrentThread());
    if (err != NO_ERROR) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "FunctionHook: DetourUpdateThread failed (err=%ld)", err);
        LogLine(buf);
        DetourTransactionAbort();
        return false;
    }

    err = DetourAttach(reinterpret_cast<PVOID*>(&g_orig),
                       reinterpret_cast<PVOID>(&hkBBuildAndAsyncSendFrame));
    if (err != NO_ERROR) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "FunctionHook: DetourAttach failed (err=%ld)", err);
        LogLine(buf);
        DetourTransactionAbort();
        return false;
    }

    err = DetourTransactionCommit();
    if (err != NO_ERROR) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "FunctionHook: DetourTransactionCommit failed (err=%ld)", err);
        LogLine(buf);
        return false;
    }

    char buf[240];
    snprintf(buf, sizeof(buf),
        "FunctionHook: BBuildAndAsyncSendFrame hooked at %p via Detours, "
        "trampoline at %p, CR forwarding ENABLED",
        target, (void*)g_orig);
    LogLine(buf);
    return true;
}

} // namespace FunctionHook
