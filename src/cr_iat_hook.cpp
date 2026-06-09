#include "cr_iat_hook.h"
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace {

// Original GetModuleHandleA pointer, captured from the IAT before we
// overwrite the slot. Used to chain through for every lookup we don't
// care about and for the fallback case (no diversion module loaded).
typedef HMODULE (WINAPI* GetModuleHandleA_t)(LPCSTR);
GetModuleHandleA_t g_origGetModuleHandleA = nullptr;

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

// The hook itself. Returns lcoverlay.dll (or diversion.dll) when CR asks
// for steamclient64.dll, otherwise chains through. The compare is
// case-insensitive because Windows treats module names that way (CR
// might compile its lookup string with any casing).
//
// On x86-64 the IAT slot is a single naturally-aligned pointer, so the
// write in Install() is atomic with respect to concurrent reads — no lock
// needed here.
HMODULE WINAPI HookedGetModuleHandleA(LPCSTR lpModuleName) {
    if (lpModuleName && _stricmp(lpModuleName, "steamclient64.dll") == 0) {
        HMODULE lcoverlay = g_origGetModuleHandleA("lcoverlay.dll");
        if (lcoverlay) return lcoverlay;
        HMODULE diversion = g_origGetModuleHandleA("diversion.dll");
        if (diversion) return diversion;
        // Neither diversion module loaded — fall through to the real lookup.
        // User probably isn't running LumaCore, or it hasn't loaded yet.
    }
    return g_origGetModuleHandleA(lpModuleName);
}

// Walk a module's import table looking for a (dll, function) pair and
// return the address of its IAT slot — the qword we'll overwrite to
// redirect calls. Handles ordinal imports by skipping them (they have no
// name to match against). Returns nullptr if either the DLL or the
// function isn't found.
void** FindIatEntry(HMODULE module, const char* importDll, const char* importFunc) {
    if (!module) return nullptr;
    auto base = reinterpret_cast<uint8_t*>(module);

    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    auto importDirRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!importDirRva) return nullptr;

    auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDirRva);
    for (; desc->Name; ++desc) {
        auto dllName = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(dllName, importDll) != 0) continue;

        // Walk OriginalFirstThunk (names) and FirstThunk (addresses) in
        // parallel. The name table is what we search; the address table
        // is what we overwrite.
        auto nameThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto addrThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; nameThunk->u1.AddressOfData; ++nameThunk, ++addrThunk) {
            if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) continue;
            auto by = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + nameThunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(by->Name), importFunc) == 0) {
                return reinterpret_cast<void**>(&addrThunk->u1.Function);
            }
        }
        return nullptr;
    }
    return nullptr;
}

}  // namespace

namespace CRIatHook {

bool Install(HMODULE cloudRedirectModule) {
    if (!cloudRedirectModule) {
        LogLine("CRIatHook: cloud_redirect module is null, cannot hook");
        return false;
    }

    // Modern Windows can bind kernel32 imports through API-set redirects
    // instead of the canonical kernel32.dll name. Try the literal name
    // first (covers most CR builds), then the API-set names that GetModule
    // currently routes through. If CR's compiler/linker picks something
    // else again, this list needs extending — but the apiset names below
    // have been stable since Win10.
    void** slot = FindIatEntry(cloudRedirectModule, "kernel32.dll", "GetModuleHandleA");
    const char* foundIn = "kernel32.dll";
    if (!slot) {
        slot = FindIatEntry(cloudRedirectModule, "api-ms-win-core-libraryloader-l1-2-0.dll", "GetModuleHandleA");
        foundIn = "api-ms-win-core-libraryloader-l1-2-0.dll";
    }
    if (!slot) {
        slot = FindIatEntry(cloudRedirectModule, "api-ms-win-core-libraryloader-l1-1-0.dll", "GetModuleHandleA");
        foundIn = "api-ms-win-core-libraryloader-l1-1-0.dll";
    }
    if (!slot) {
        LogLine("CRIatHook: GetModuleHandleA IAT entry not found in cloud_redirect.dll's imports. "
                "CR may have been built with a different import binding — needs investigation.");
        return false;
    }

    g_origGetModuleHandleA = reinterpret_cast<GetModuleHandleA_t>(*slot);

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        LogLine("CRIatHook: VirtualProtect (RW) on the IAT slot failed");
        return false;
    }
    *slot = reinterpret_cast<void*>(&HookedGetModuleHandleA);
    DWORD dummy = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &dummy);

    char buf[400];
    snprintf(buf, sizeof(buf),
        "CRIatHook: GetModuleHandleA hooked at IAT slot %p (imported via %s). "
        "Original=%p, our hook=%p. CR's lookups for \"steamclient64.dll\" "
        "will be redirected to lcoverlay.dll (diversion.dll fallback).",
        slot, foundIn, (void*)g_origGetModuleHandleA, (void*)&HookedGetModuleHandleA);
    LogLine(buf);
    return true;
}

}  // namespace CRIatHook
