#pragma once
#include <windows.h>
#include <cstdint>

namespace CRLoader {
    using CloudOnSendPkt_t = int(*)(void* thisptr, const uint8_t* data, uint32_t size, void* recvPktFn);

    // Loads cloud_redirect.dll and resolves CloudOnSendPkt.
    // Logs results to %TEMP%\crbridge.log and OutputDebugString.
    // Returns true if both LoadLibrary and GetProcAddress succeeded.
    bool TryLoad();

    // Returns the resolved CloudOnSendPkt function, or nullptr if not loaded.
    CloudOnSendPkt_t GetCloudOnSendPkt();
}
