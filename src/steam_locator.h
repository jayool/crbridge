#pragma once
#include <windows.h>
#include <cstdint>

namespace SteamLocator {
    HMODULE WaitForSteamClient(int maxMs);
    const void* FindServiceTransportRttiString();

    const void* TypeDescriptorFromName(const void* nameAddr);

    int FindColsReferencingTypeDescriptor(HMODULE module, const void* typeDescriptor,
                                          const void** outResults, int maxResults);

    const void* FindVtableForCol(HMODULE module, const void* col);

    void DumpVtable(const void* vtable, int n);

    bool DiagnoseRTTI();

    // === Iteration 7 ===
    // Pattern format: "48 8B C4 55" (uppercase hex, space-separated). Use "??" for wildcards.
    const void* FindPattern(const uint8_t* haystack, size_t haystackSize, const char* pattern);

    // Find Steam's BBuildAndAsyncSendFrame using LumaCore's known patterns.
    const void* FindBBuildAndAsyncSendFrame();
}
