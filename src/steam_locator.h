#pragma once
#include <windows.h>
#include <cstdint>

namespace SteamLocator {
    HMODULE WaitForSteamClient(int maxMs);
    const void* FindServiceTransportRttiString();

    // === Iteration 4: RTTI walk ===

    // Returns address of the TypeDescriptor enclosing the given RTTI name string.
    const void* TypeDescriptorFromName(const void* nameAddr);

    // Scans steamclient64.dll for Complete Object Locators referencing the given
    // TypeDescriptor. Writes up to maxResults addresses into outResults.
    // Returns the number of COLs found.
    int FindColsReferencingTypeDescriptor(HMODULE module, const void* typeDescriptor,
                                          const void** outResults, int maxResults);

    // Scans steamclient64.dll for a vtable whose -8 offset (a "RTTI pointer slot")
    // points to the given COL. Returns the vtable address (right after the slot)
    // or nullptr if not found.
    const void* FindVtableForCol(HMODULE module, const void* col);

    // Logs the first n slots of the given vtable.
    void DumpVtable(const void* vtable, int n);

    // Runs the full Iteration 4 diagnostic.
    bool DiagnoseRTTI();
}
