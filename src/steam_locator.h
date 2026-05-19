#pragma once
#include <windows.h>
#include <cstdint>

namespace SteamLocator {
    // Base address of steamclient64.dll (waits up to 10s for it to load).
    // Returns nullptr if not loaded within that window.
    HMODULE WaitForSteamClient(int maxMs);

    // Searches steamclient64.dll for the RTTI type string
    // ".?AVCClientUnifiedServiceTransport@@". Returns its address or nullptr.
    // Logs progress to %TEMP%\crbridge.log.
    const void* FindServiceTransportRttiString();

    // Runs the full Iteration 3 diagnostic. Returns true if the RTTI string was found.
    bool Diagnose();
}
