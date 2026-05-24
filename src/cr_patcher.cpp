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

    // The pObject we receive in BBuildAndAsyncSendFrame is actually a
    // CWebSocketConnection (verified by RTTI: ".?AVCWebSocketConnection@@"),
    // not a CCMInterface. CCMInterface owns one or more CWebSocketConnections
    // and they hold a back-pointer to their owner somewhere in their fields.
    //
    // Scan the first kScanBytes of the candidate's struct, treating each
    // qword as a potential pointer; for each one that resolves to readable
    // memory, check if *that* memory's first qword equals CCMInterface::vftable.
    // The first such hit is almost certainly the owning CCMInterface — vtable
    // identity is unique enough across a process that random qword aliasing
    // is statistically negligible.
    //
    // Returns nullptr if no plausible match is found.
    void* DeriveCmInterfaceFromCandidate(void* pCandidate, void* expectedCcmVtable) {
        if (!IsReadable(pCandidate, sizeof(void*))) return nullptr;
        constexpr size_t kScanBytes = 4096;
        auto* base = static_cast<uintptr_t*>(pCandidate);
        size_t kScanQwords = kScanBytes / sizeof(uintptr_t);
        for (size_t i = 1; i < kScanQwords; ++i) {
            if (!IsReadable(&base[i], sizeof(void*))) continue;
            auto* candidate = reinterpret_cast<void*>(base[i]);
            if (!IsReadable(candidate, sizeof(void*))) continue;
            void* candidateVt = *static_cast<void**>(candidate);
            if (candidateVt == expectedCcmVtable) {
                return candidate;
            }
        }
        return nullptr;
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

    // CRITICAL: When LumaCore is present, it copies steamclient64.dll to
    // lcoverlay.dll (or legacy diversion.dll) and intercepts Steam's internal
    // LoadModuleWithPath so that Steam uses the COPY at runtime. That means
    // every running CCMInterface, CWebSocketConnection, etc. has a vtable
    // pointer into the COPY's image, NOT the original steamclient64.dll's
    // image. We must therefore base all our RVA arithmetic on whichever
    // module is actually backing the runtime objects.
    //
    // Without this, the vtable check below would never match (we'd search
    // for sc+0x128E7A0 but the running objects have lcoverlay+0x128E7A0),
    // and the member-scan derivation would fail for the same reason.
    HMODULE effective = GetModuleHandleA("lcoverlay.dll");
    const char* effectiveName = "lcoverlay.dll";
    if (!effective) {
        effective = GetModuleHandleA("diversion.dll");
        effectiveName = "diversion.dll";
    }
    if (!effective) {
        effective = sc;
        effectiveName = "steamclient64.dll (no LumaCore diversion found)";
    }

    // The pObject from BBuildAndAsyncSendFrame is actually a
    // CWebSocketConnection (RTTI-verified), not a CCMInterface. Verify by
    // checking its vtable against CCMInterface::vftable in the effective
    // module; if it doesn't match (the expected case), scan its members
    // for the owning CCMInterface back-pointer.
    void* expectedVtable = reinterpret_cast<char*>(effective) + SC_RVA_CCMI_VTABLE;
    if (!IsReadable(candidateCmInterface, sizeof(void*))) {
        return false;
    }

    void* resolvedCm = candidateCmInterface;
    void* actualVtable = *static_cast<void**>(candidateCmInterface);
    if (actualVtable != expectedVtable) {
        resolvedCm = DeriveCmInterfaceFromCandidate(candidateCmInterface, expectedVtable);
        if (!resolvedCm) {
            // Couldn't find a CCMInterface back-pointer in this candidate's
            // first 4KB of fields. Try the next packet's pObject. Log only the
            // first few attempts to avoid spam.
            static std::atomic<int> derivationFailLogCount{0};
            int n = derivationFailLogCount.fetch_add(1);
            if (n < 3) {
                char buf[480];
                snprintf(buf, sizeof(buf),
                    "CRPatcher: candidate %p has wrong vtable (actual=%p expected=%p, "
                    "using effective base %s at %p) and no embedded CCMInterface "
                    "back-pointer found in first 4096 bytes — will retry on next packet (log #%d)",
                    candidateCmInterface, actualVtable, expectedVtable,
                    effectiveName, (void*)effective, n + 1);
                LogLine(buf);
            }
            return false;
        }
        // Compute the offset where we found the back-pointer, for diagnostics.
        ptrdiff_t derivedOffset = -1;
        {
            auto* bytes = reinterpret_cast<uintptr_t*>(candidateCmInterface);
            for (size_t i = 0; i < 512; ++i) {
                if (reinterpret_cast<void*>(bytes[i]) == resolvedCm) {
                    derivedOffset = static_cast<ptrdiff_t>(i * sizeof(uintptr_t));
                    break;
                }
            }
        }
        char buf[480];
        snprintf(buf, sizeof(buf),
            "CRPatcher: derived CCMInterface %p from CWebSocketConnection %p "
            "at member offset +0x%llX (vtable matches %s + 0x%llX)",
            resolvedCm, candidateCmInterface,
            static_cast<unsigned long long>(derivedOffset),
            effectiveName,
            static_cast<unsigned long long>(SC_RVA_CCMI_VTABLE));
        LogLine(buf);
    }

    // Acquire lock and re-check (double-checked locking).
    std::lock_guard<std::mutex> lock(g_patchMutex);
    if (g_patched.load(std::memory_order_acquire)) {
        return true;
    }

    char* crBase = reinterpret_cast<char*>(cr);
    char* effBase = reinterpret_cast<char*>(effective);

    void* wrapPacket   = effBase + SC_RVA_WRAP_PACKET;
    void* bRouteMsg    = effBase + SC_RVA_BROUTE_MSG;
    void* releaseWrap  = effBase + SC_RVA_RELEASE_WRAPPED;

    bool ok = true;
    // CR's INJECT path reads g_steamclient64_base for several RVA computations;
    // setting it to the effective (diverted) module keeps everything consistent
    // within one image's address space.
    ok &= WriteProtectedPointer(crBase + CR_RVA_STEAMCLIENT_BASE, effective);
    ok &= WriteProtectedPointer(crBase + CR_RVA_CMINTERFACE,      resolvedCm);
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
        "effective_base=%s@%p (sc_orig=%p) cr=%p cmInterface=%p "
        "(vtable matches CCMInterface::vftable @ effective+0x%llX) "
        "wrapPacket=%p bRouteMsgToJob=%p releaseWrapped=%p init_flag=1",
        effectiveName, (void*)effective, (void*)sc, (void*)cr, resolvedCm,
        (unsigned long long)SC_RVA_CCMI_VTABLE,
        wrapPacket, bRouteMsg, releaseWrap);
    LogLine(buf);

    g_patched.store(true, std::memory_order_release);
    return true;
}

} // namespace CRPatcher
