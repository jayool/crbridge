#pragma once
#include <windows.h>

namespace SteamLocator {

// Block until steamclient64.dll loads (either Valve's own copy or the one
// LumaCore loads as part of its proxy chain) or `maxMs` ms have passed.
// Returns the module handle or nullptr on timeout.
HMODULE WaitForSteamClient(int maxMs);

// Best-effort wait for the LumaCore diverted module — the renamed copy of
// steamclient64.dll where Steam's runtime code actually executes. Modern
// SteaMidra calls it `lcoverlay.dll`, the legacy name was `diversion.dll`.
//
// `outFoundName` (optional) receives a pointer to a static string with
// the name that matched, useful for logging.
//
// Returns nullptr on timeout. Callers should treat that as "user isn't
// running LumaCore" and proceed with the IAT hook installed anyway — the
// hook will fall through to the original GetModuleHandleA, which keeps
// behaviour correct in non-LumaCore environments.
HMODULE WaitForDiversion(int maxMs, const char** outFoundName);

}  // namespace SteamLocator
