#pragma once
#include <windows.h>

namespace HostAlias {
    // Hooks GetModuleHandleA/W so lookups for "diversion.dll" resolve to
    // `diverted` (SteaMidra's bin\lcoverlay.dll). This lets an unmodified
    // ost-branch cloud_redirect.dll self-init against SteaMidra without
    // patching the binary, so CR auto-updates keep working. Idempotent.
    bool Install(HMODULE diverted);
}
