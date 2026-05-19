#pragma once
#include <windows.h>

namespace VtableHook {
    // Installs hooks on all 10 slots of the given vtable.
    // Each hook logs the first kMaxLogPerSlot calls with its arguments
    // and then silently forwards to the original. Returns true on success.
    bool Install(const void* vtable);
}
