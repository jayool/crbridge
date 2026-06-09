#pragma once
#include <windows.h>

namespace CRIatHook {

// Install an IAT hook on `cloud_redirect.dll`'s GetModuleHandleA import so
// that when CR calls GetModuleHandleA("steamclient64.dll"), we transparently
// return the handle of LumaCore's diverted module (`lcoverlay.dll`, falling
// back to legacy `diversion.dll`). CR's internal RVA arithmetic then
// resolves CCMInterface and the cloud-RPC vtable inside the live runtime
// module instead of the dormant vanilla copy, which is what makes
// CR_InitCloudSave succeed in a LumaCore-diverted environment.
//
// Returns true on success. On failure, the log explains which step failed
// (no IAT entry found, VirtualProtect denied, etc.) and CR will see vanilla
// steamclient64.dll — which is the correct behavior outside LumaCore but
// fails to find the runtime CCMInterface in LumaCore mode.
//
// Must be called BEFORE the host invokes CR_InitCloudSave so the very first
// lookup hits the redirected path.
bool Install(HMODULE cloudRedirectModule);

}  // namespace CRIatHook
