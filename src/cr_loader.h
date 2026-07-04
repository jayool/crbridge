#pragma once
#include <windows.h>
#include <cstdint>

namespace CRLoader {

// Notification callback the host can pass to CR_InitCloudSave. Signature
// must match cr_api.h. We always pass nullptr (CR defaults to MessageBoxA)
// for now; if we ever want to route notifications somewhere else (e.g.
// the crbridge log), we plug a real callback here.
typedef void (*CR_NotifyFn)(int level, const char* title, const char* message);

// The v2.1.6+ exports we use. Types must match cr_api.h exactly.
typedef bool (*CR_InitCloudSave_t)(const char* steamPath, CR_NotifyFn notify);
typedef void (*CR_SetApps_t)(const uint32_t* appIds, uint32_t count);
typedef void (*CR_Shutdown_t)();

// CR >= 2.2.5. Arms CR's cloud-RPC interception on Steam's transport vtable
// and returns whether the hook is active. Older CR (<= 2.1.x) installed the
// hook *inside* CR_InitCloudSave, so on those builds this export is absent —
// resolve it best-effort and only call it when present.
typedef bool (*CR_InstallVtableHooks_t)();

// Loads cloud_redirect.dll (next to steam.exe, then via system search) and
// resolves the required exports. Returns false if either step fails.
bool TryLoad();

// Accessors. Return nullptr if TryLoad hasn't run or the export wasn't
// found. CR_InitCloudSave is required; the others are best-effort.
CR_InitCloudSave_t GetInitCloudSave();
CR_SetApps_t       GetSetApps();
CR_Shutdown_t      GetShutdown();
CR_InstallVtableHooks_t GetInstallVtableHooks();

// Module handle of the loaded cloud_redirect.dll. Needed by CRIatHook so
// it can find the IAT to patch.
HMODULE GetModule();

}  // namespace CRLoader
