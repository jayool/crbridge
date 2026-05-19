#include "vtable_hook.h"
#include <cstdio>
#include <atomic>
#include <cstdint>

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

    // Generic 8-arg function pointer to forward calls without losing arguments.
    // Covers up to 8 integer/pointer args (4 in registers + 4 on stack).
    // Sufficient for any reasonable Steam virtual method.
    using GenericFn_t = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                       uintptr_t, uintptr_t, uintptr_t, uintptr_t);

    GenericFn_t g_origs[10] = {};
    std::atomic<int> g_hitCounts[10] = {};
    constexpr int kMaxLogPerSlot = 5;

    template <int Slot>
    uintptr_t HookFn(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4,
                     uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8) {
        int count = g_hitCounts[Slot].fetch_add(1) + 1;
        if (count <= kMaxLogPerSlot) {
            char buf[320];
            snprintf(buf, sizeof(buf),
                "VT[%2d] HIT #%d: this=%p arg2=%p arg3=%p arg4=%p",
                Slot, count, (void*)a1, (void*)a2, (void*)a3, (void*)a4);
            LogLine(buf);
        }
        return g_origs[Slot](a1, a2, a3, a4, a5, a6, a7, a8);
    }
}

namespace VtableHook {

bool Install(const void* vtable) {
    if (!vtable) return false;

    uintptr_t* p = const_cast<uintptr_t*>(reinterpret_cast<const uintptr_t*>(vtable));
    constexpr size_t kSize = 10 * sizeof(void*);

    DWORD oldProtect = 0;
    if (!VirtualProtect(p, kSize, PAGE_READWRITE, &oldProtect)) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "VtableHook: VirtualProtect failed (error %lu)", GetLastError());
        LogLine(buf);
        return false;
    }

    g_origs[0] = reinterpret_cast<GenericFn_t>(p[0]); p[0] = reinterpret_cast<uintptr_t>(&HookFn<0>);
    g_origs[1] = reinterpret_cast<GenericFn_t>(p[1]); p[1] = reinterpret_cast<uintptr_t>(&HookFn<1>);
    g_origs[2] = reinterpret_cast<GenericFn_t>(p[2]); p[2] = reinterpret_cast<uintptr_t>(&HookFn<2>);
    g_origs[3] = reinterpret_cast<GenericFn_t>(p[3]); p[3] = reinterpret_cast<uintptr_t>(&HookFn<3>);
    g_origs[4] = reinterpret_cast<GenericFn_t>(p[4]); p[4] = reinterpret_cast<uintptr_t>(&HookFn<4>);
    g_origs[5] = reinterpret_cast<GenericFn_t>(p[5]); p[5] = reinterpret_cast<uintptr_t>(&HookFn<5>);
    g_origs[6] = reinterpret_cast<GenericFn_t>(p[6]); p[6] = reinterpret_cast<uintptr_t>(&HookFn<6>);
    g_origs[7] = reinterpret_cast<GenericFn_t>(p[7]); p[7] = reinterpret_cast<uintptr_t>(&HookFn<7>);
    g_origs[8] = reinterpret_cast<GenericFn_t>(p[8]); p[8] = reinterpret_cast<uintptr_t>(&HookFn<8>);
    g_origs[9] = reinterpret_cast<GenericFn_t>(p[9]); p[9] = reinterpret_cast<uintptr_t>(&HookFn<9>);

    DWORD dummy = 0;
    VirtualProtect(p, kSize, oldProtect, &dummy);
    FlushInstructionCache(GetCurrentProcess(), p, kSize);

    LogLine("VtableHook: installed hooks on all 10 slots");
    return true;
}

} // namespace VtableHook
