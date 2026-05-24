#include "cr_patcher.h"
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <mutex>

namespace {

    // ---- RVA constants ------------------------------------------------------
    //
    // The cloud_redirect.dll RVAs are tied to the specific build verified by
    // static analysis on 2026-05-24: md5 0ee5a330df13c226f09e94fc39bf9089,
    // 1,230,848 bytes, supports Steam 1779486452 / 1778281814 / 1778003620.
    // The steamclient64.dll RVAs come from cloud_redirect.dll's own hardcoded
    // `lea reg, [base + RVA]` instructions in its setter (sub_180BAB70) — so
    // they are correct *for whatever Steam builds CR was compiled to support*.
    //
    // When either DLL updates, re-run the disasm pass documented in
    // memory/cloud_redirect_contract.md to refresh these constants.

    // cloud_redirect.dll: where the 6 globals live in .data
    constexpr uintptr_t CR_RVA_STEAMCLIENT_BASE = 0x126C30;  // HMODULE cache
    constexpr uintptr_t CR_RVA_CMINTERFACE      = 0x126C48;  // void*
    constexpr uintptr_t CR_RVA_INIT_FLAG        = 0x126C51;  // byte: setter-done flag (NOT 0x126C50!)
    constexpr uintptr_t CR_RVA_WRAP_PACKET      = 0x126CD8;  // fn ptr
    constexpr uintptr_t CR_RVA_BROUTE_MSG       = 0x126CE0;  // fn ptr
    constexpr uintptr_t CR_RVA_RELEASE_WRAPPED  = 0x126CE8;  // fn ptr

    // steamclient64.dll: offsets to the helpers CR needs
    constexpr uintptr_t SC_RVA_WRAP_PACKET     = 0xD199E0;
    constexpr uintptr_t SC_RVA_BROUTE_MSG      = 0xD263B0;
    constexpr uintptr_t SC_RVA_RELEASE_WRAPPED = 0xEB760;
    constexpr uintptr_t SC_RVA_CCMI_VTABLE     = 0x128E7A0;  // CCMInterface::vftable

    // ---- Synchronization ----------------------------------------------------

    std::atomic<bool> g_patched{false};
    std::mutex g_patchMutex;

    // ---- Logging (local copy of the pattern used in other modules) ----------

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

    // ---- Safe memory writes -------------------------------------------------

    bool WriteProtectedPointer(void* dst, void* value) {
        DWORD oldProtect = 0;
        if (!VirtualProtect(dst, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            return false;
        }
        *static_cast<void**>(dst) = value;
        DWORD dummy = 0;
        VirtualProtect(dst, sizeof(void*), oldProtect, &dummy);
        return true;
    }

    bool WriteProtectedByte(void* dst, uint8_t value) {
        DWORD oldProtect = 0;
        if (!VirtualProtect(dst, 1, PAGE_READWRITE, &oldProtect)) {
            return false;
        }
        *static_cast<uint8_t*>(dst) = value;
        DWORD dummy = 0;
        VirtualProtect(dst, 1, oldProtect, &dummy);
        return true;
    }

    // ---- Candidate validation -----------------------------------------------

    // Returns true if `p` points at committed, readable memory of at least
    // `bytes` bytes. Used to guard the vtable-read against a junk pObject.
    bool IsReadable(const void* p, size_t bytes) {
        if (!p) return false;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        constexpr DWORD readMask = PAGE_READONLY | PAGE_READWRITE |
                                   PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!(mbi.Protect & readMask)) return false;
        auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        auto end  = base + mbi.RegionSize;
        auto need = reinterpret_cast<uintptr_t>(p) + bytes;
        return need <= end;
    }

} // namespace

namespace CRPatcher {

bool TryPatch(void* candidateCmInterface) {
    // Fast path: already patched, nothing to do.
    if (g_patched.load(std::memory_order_acquire)) {
        return true;
    }

    HMODULE sc = GetModuleHandleA("steamclient64.dll");
    HMODULE cr = GetModuleHandleA("cloud_redirect.dll");
    if (!sc || !cr) {
        // Not an error — modules may not be loaded yet on early BBAASF
        // calls. Caller will retry next packet.
        return false;
    }

    // Verify candidate's vtable BEFORE taking the lock. This is the
    // same check CR's own `sub_180A9FD0` performs at +0x48 of the user
    // struct; here we apply it to whatever pObject we received.
    if (!IsReadable(candidateCmInterface, sizeof(void*))) {
        return false;
    }
    void* actualVtable = *static_cast<void**>(candidateCmInterface);
    void* expectedVtable = reinterpret_cast<char*>(sc) + SC_RVA_CCMI_VTABLE;
    if (actualVtable != expectedVtable) {
        // Not the right kind of object — caller will retry with a later pObject.
        // Log only first few mismatches to avoid spamming.
        static std::atomic<int> mismatchLogCount{0};
        int n = mismatchLogCount.fetch_add(1);
        if (n < 3) {
            char buf[320];
            snprintf(buf, sizeof(buf),
                "CRPatcher: vtable mismatch for candidate %p (expected=%p actual=%p) "
                "— will retry on next packet (log #%d)",
                candidateCmInterface, expectedVtable, actualVtable, n + 1);
            LogLine(buf);
        }
        return false;
    }

    // Acquire lock and re-check (double-checked locking).
    std::lock_guard<std::mutex> lock(g_patchMutex);
    if (g_patched.load(std::memory_order_acquire)) {
        return true;
    }

    char* crBase = reinterpret_cast<char*>(cr);
    char* scBase = reinterpret_cast<char*>(sc);

    void* wrapPacket   = scBase + SC_RVA_WRAP_PACKET;
    void* bRouteMsg    = scBase + SC_RVA_BROUTE_MSG;
    void* releaseWrap  = scBase + SC_RVA_RELEASE_WRAPPED;

    bool ok = true;
    ok &= WriteProtectedPointer(crBase + CR_RVA_STEAMCLIENT_BASE, sc);
    ok &= WriteProtectedPointer(crBase + CR_RVA_CMINTERFACE,      candidateCmInterface);
    ok &= WriteProtectedPointer(crBase + CR_RVA_WRAP_PACKET,      wrapPacket);
    ok &= WriteProtectedPointer(crBase + CR_RVA_BROUTE_MSG,       bRouteMsg);
    ok &= WriteProtectedPointer(crBase + CR_RVA_RELEASE_WRAPPED,  releaseWrap);
    ok &= WriteProtectedByte   (crBase + CR_RVA_INIT_FLAG,        1);

    if (!ok) {
        LogLine("CRPatcher: VirtualProtect failed during patching, globals possibly partial");
        return false;
    }

    char buf[640];
    snprintf(buf, sizeof(buf),
        "CRPatcher: PATCHED successfully. "
        "sc=%p cr=%p cmInterface=%p (vtable matches CCMInterface::vftable @ +0x%llX) "
        "wrapPacket=%p bRouteMsgToJob=%p releaseWrapped=%p init_flag=1",
        (void*)sc, (void*)cr, candidateCmInterface,
        (unsigned long long)SC_RVA_CCMI_VTABLE,
        wrapPacket, bRouteMsg, releaseWrap);
    LogLine(buf);

    g_patched.store(true, std::memory_order_release);
    return true;
}

} // namespace CRPatcher
