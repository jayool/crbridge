#pragma once
#include <windows.h>

namespace CRLoader {
    // Loads cloud_redirect.dll from next to steam.exe (falling back to the
    // search path). The ost-branch CR self-initializes on load, so no export
    // call is needed. Returns true if the DLL was loaded.
    bool TryLoad();
}
