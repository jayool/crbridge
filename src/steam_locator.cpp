#include "steam_locator.h"
#include "vtable_hook.h"
#include <cstdio>
#include <cstring>

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

    struct PESection {
        const uint8_t* base;
        size_t size;
    };

    bool FindSection(HMODULE module, const char* name, PESection* out) {
        if (!module) return false;
        auto base = reinterpret_cast<const uint8_t*>(module);
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        auto section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            if (strncmp(reinterpret_cast<const char*>(section->Name), name, 8) == 0) {
                out->base = base + section->VirtualAddress;
                out->size = section->Misc.VirtualSize;
                return true;
            }
        }
        return false;
    }

    const void* SearchBytes(const uint8_t* haystack, size_t haystackSize,
                            const char* needle, size_t needleSize) {
        if (needleSize == 0 || needleSize > haystackSize) return nullptr;
        for (size_t i = 0; i <= haystackSize - needleSize; ++i) {
            if (memcmp(haystack + i, needle, needleSize) == 0) {
                return haystack + i;
            }
        }
        return nullptr;
    }
}

namespace SteamLocator {

HMODULE WaitForSteamClient(int maxMs) {
    const int sleepStep = 100;
    int waited = 0;
    while (waited < maxMs) {
        HMODULE h = GetModuleHandleA("steamclient64.dll");
        if (h) return h;
        Sleep(sleepStep);
        waited += sleepStep;
    }
    return nullptr;
}

const void* FindServiceTransportRttiString() {
    HMODULE sc = WaitForSteamClient(10000);
    if (!sc) {
        LogLine("SteamLocator: steamclient64.dll not loaded after 10s wait");
        return nullptr;
    }

    char buf[640];
    snprintf(buf, sizeof(buf),
        "SteamLocator: steamclient64.dll base at %p", (void*)sc);
    LogLine(buf);

    const char* target = ".?AVCClientUnifiedServiceTransport@@";
    size_t targetLen = strlen(target);

    const char* sections[] = {".data", ".rdata"};
    for (const char* sectionName : sections) {
        PESection sec = {};
        if (!FindSection(sc, sectionName, &sec)) continue;
        snprintf(buf, sizeof(buf),
            "SteamLocator: %s at %p size %zu bytes",
            sectionName, (void*)sec.base, sec.size);
        LogLine(buf);
        const void* found = SearchBytes(sec.base, sec.size, target, targetLen);
        if (found) {
            snprintf(buf, sizeof(buf),
                "SteamLocator: RTTI string found at %p (section %s)",
                found, sectionName);
            LogLine(buf);
            return found;
        }
    }
    LogLine("SteamLocator: RTTI string NOT FOUND");
    return nullptr;
}

const void* TypeDescriptorFromName(const void* nameAddr) {
    return reinterpret_cast<const uint8_t*>(nameAddr) - 16;
}

int FindColsReferencingTypeDescriptor(HMODULE module, const void* typeDescriptor,
                                      const void** outResults, int maxResults) {
    if (!module || !typeDescriptor) return 0;

    auto base = reinterpret_cast<uintptr_t>(module);
    auto tdAddr = reinterpret_cast<uintptr_t>(typeDescriptor);
    if (tdAddr < base) return 0;
    uint32_t tdRva = static_cast<uint32_t>(tdAddr - base);

    char buf[640];
    snprintf(buf, sizeof(buf),
        "RTTI: TypeDescriptor RVA = 0x%08X, scanning for COLs", tdRva);
    LogLine(buf);

    int found = 0;
    const char* sections[] = {".rdata", ".data"};
    for (const char* secName : sections) {
        PESection sec = {};
        if (!FindSection(module, secName, &sec)) continue;

        const uint8_t* end = sec.base + sec.size;
        for (const uint8_t* p = sec.base; p + 24 <= end; p += 4) {
            const uint32_t* col = reinterpret_cast<const uint32_t*>(p);
            if (col[0] != 1) continue;
            if (col[3] != tdRva) continue;
            uint32_t selfRva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p) - base);
            if (col[5] != selfRva) continue;

            snprintf(buf, sizeof(buf),
                "RTTI: COL #%d found at %p (section %s)", found, (void*)p, secName);
            LogLine(buf);

            if (found < maxResults) {
                outResults[found++] = p;
            }
        }
    }

    if (found == 0) {
        LogLine("RTTI: no COLs found referencing this TypeDescriptor");
    }
    return found;
}

const void* FindVtableForCol(HMODULE module, const void* col) {
    if (!module || !col) return nullptr;
    auto colAddr = reinterpret_cast<uintptr_t>(col);

    const char* sections[] = {".rdata", ".data"};
    for (const char* secName : sections) {
        PESection sec = {};
        if (!FindSection(module, secName, &sec)) continue;
        const uint8_t* end = sec.base + sec.size;
        for (const uint8_t* p = sec.base; p + 16 <= end; p += 8) {
            uintptr_t value;
            memcpy(&value, p, sizeof(value));
            if (value == colAddr) {
                const void* vtable = p + 8;
                char buf[640];
                snprintf(buf, sizeof(buf),
                    "RTTI: vtable for COL %p found at %p (section %s)",
                    col, vtable, secName);
                LogLine(buf);
                return vtable;
            }
        }
    }
    LogLine("RTTI: no vtable found for this COL");
    return nullptr;
}

void DumpVtable(const void* vtable, int n) {
    if (!vtable) return;
    auto p = reinterpret_cast<const uintptr_t*>(vtable);
    char buf[640];
    for (int i = 0; i < n; ++i) {
        uintptr_t fnAddr = p[i];
        char hexBuf[80] = {};

        MEMORY_BASIC_INFORMATION mbi = {};
        bool readable = false;
        if (VirtualQuery(reinterpret_cast<LPCVOID>(fnAddr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
            if (mbi.State == MEM_COMMIT &&
                (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                 PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
                readable = true;
            }
        }

        if (readable) {
            const uint8_t* fn = reinterpret_cast<const uint8_t*>(fnAddr);
            for (int j = 0; j < 16; ++j) {
                char tmp[4];
                snprintf(tmp, sizeof(tmp), "%02X ", fn[j]);
                strcat_s(hexBuf, sizeof(hexBuf), tmp);
            }
        } else {
            strcpy_s(hexBuf, sizeof(hexBuf), "(not executable memory)");
        }

        snprintf(buf, sizeof(buf),
            "RTTI: vtable[%2d] = %p  prologue: %s", i, (void*)fnAddr, hexBuf);
        LogLine(buf);
    }
}

const void* FindPattern(const uint8_t* haystack, size_t haystackSize,
                        const char* pattern) {
    uint8_t bytes[512] = {};
    bool mask[512] = {};
    int n = 0;

    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };

    const char* p = pattern;
    while (*p && n < 512) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') {
            mask[n] = false;
            bytes[n] = 0;
            n++;
            p += 2;
        } else {
            int high = hexVal(p[0]);
            int low = hexVal(p[1]);
            if (high < 0 || low < 0) return nullptr;
            bytes[n] = static_cast<uint8_t>((high << 4) | low);
            mask[n] = true;
            n++;
            p += 2;
        }
    }
    if (n == 0 || static_cast<size_t>(n) > haystackSize) return nullptr;

    for (size_t i = 0; i + n <= haystackSize; ++i) {
        bool match = true;
        for (int j = 0; j < n; ++j) {
            if (mask[j] && haystack[i + j] != bytes[j]) {
                match = false;
                break;
            }
        }
        if (match) return haystack + i;
    }
    return nullptr;
}

static const void* FindBBuildAndAsyncInModule(HMODULE module, const char* moduleLabel) {
    PESection text = {};
    if (!FindSection(module, ".text", &text)) {
        char buf[200];
        snprintf(buf, sizeof(buf),
            "BBuildAndAsync: .text section not found in %s", moduleLabel);
        LogLine(buf);
        return nullptr;
    }

    struct Sig { const char* label; const char* pattern; };
    const Sig sigs[] = {
        {"1778803745 (beta)",
         "48 8B C4 55 48 8D 68 A1 48 81 EC C0 00 00 00 48 89 70 18"},
        {"1778281814 (stable)",
         "48 8B C4 55 48 8D 68 A1 48 81 EC C0 00 00 00"},
    };

    char buf[400];
    for (const auto& sig : sigs) {
        const void* found = FindPattern(text.base, text.size, sig.pattern);
        if (found) {
            snprintf(buf, sizeof(buf),
                "BBuildAndAsync: FOUND in %s at %p (sig: %s)",
                moduleLabel, found, sig.label);
            LogLine(buf);
            return found;
        }
    }
    return nullptr;
}

const void* FindBBuildAndAsyncSendFrame() {
    HMODULE sc = WaitForSteamClient(10000);
    if (!sc) return nullptr;

    const void* originalAddr = FindBBuildAndAsyncInModule(sc, "steamclient64.dll");
    if (!originalAddr) return nullptr;

    uintptr_t rva = reinterpret_cast<uintptr_t>(originalAddr) - reinterpret_cast<uintptr_t>(sc);

    char buf[400];

    HMODULE diversion = GetModuleHandleA("diversion.dll");
    if (diversion) {
        const void* diversionAddr = reinterpret_cast<const void*>(
            reinterpret_cast<uintptr_t>(diversion) + rva);
        snprintf(buf, sizeof(buf),
            "BBuildAndAsync: diversion.dll detected at %p; "
            "mapping function via RVA 0x%llX -> %p (preferring diversion for hook)",
            (void*)diversion, (unsigned long long)rva, diversionAddr);
        LogLine(buf);
        return diversionAddr;
    }

    snprintf(buf, sizeof(buf),
        "BBuildAndAsync: no diversion.dll, using original at %p (RVA 0x%llX)",
        originalAddr, (unsigned long long)rva);
    LogLine(buf);
    return originalAddr;
}

bool DiagnoseRTTI() {
    const void* rttiStr = FindServiceTransportRttiString();
    if (!rttiStr) return false;

    const void* td = TypeDescriptorFromName(rttiStr);
    char buf[640];
    snprintf(buf, sizeof(buf), "RTTI: TypeDescriptor at %p (string - 16)", td);
    LogLine(buf);

    HMODULE sc = GetModuleHandleA("steamclient64.dll");
    if (!sc) return false;

    const void* cols[8] = {};
    int nCols = FindColsReferencingTypeDescriptor(sc, td, cols, 8);
    if (nCols == 0) return false;

    for (int i = 0; i < nCols; ++i) {
        const void* vtable = FindVtableForCol(sc, cols[i]);
        if (vtable) {
            DumpVtable(vtable, 16);
        }
    }

    FindBBuildAndAsyncSendFrame();
    return true;
}

} // namespace SteamLocator
