#include "steam_locator.h"
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
        if (!FindSection(sc, sectionName, &sec)) {
            snprintf(buf, sizeof(buf),
                "SteamLocator: section %s not found", sectionName);
            LogLine(buf);
            continue;
        }
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

    LogLine("SteamLocator: RTTI string '.?AVCClientUnifiedServiceTransport@@' NOT FOUND");
    return nullptr;
}

bool Diagnose() {
    return FindServiceTransportRttiString() != nullptr;
}

} // namespace SteamLocator
