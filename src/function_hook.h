#pragma once
#include <windows.h>

namespace FunctionHook {
    // Installs MinHook globally + hooks BBuildAndAsyncSendFrame at the given address.
    // Returns true if both MinHook init and hook install succeeded.
    bool InstallBBuildAndAsyncSendFrame(void* target);
}
