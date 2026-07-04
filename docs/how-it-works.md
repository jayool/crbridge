# How crbridge works

crbridge is a small loader: it gets CloudRedirect (CR) running and correctly
targeted inside a Steam process that is **not** running SteamTools. Two problems
have to be solved for that — locating Steam's live runtime, and arming CR's
interception — and each maps to one piece of crbridge.

## 1. Locating the live runtime: the IAT redirect

CR's entry point is `CR_InitCloudSave(steamPath, notifyCallback)`. Internally CR
calls `GetModuleHandleA("steamclient64.dll")` and walks Steam's `CSteamEngine`
global to find the current `CCMInterface` — the cloud-RPC dispatcher it hooks.

That lookup breaks in a **LumaCore-diverted** Steam. LumaCore runs the live
runtime from `lcoverlay.dll` — a renamed copy of `steamclient64.dll` where it
installs its own hooks. The vanilla `steamclient64.dll` is still mapped into the
process (so `GetModuleHandleA` happily returns its handle) but holds no live
runtime objects. CR walks that dormant copy, never finds `CCMInterface`, and
`CR_InitCloudSave` returns `false`.

crbridge fixes this by overwriting `cloud_redirect.dll`'s Import Address Table
entry for `GetModuleHandleA` with a thunk:

- Module name is `"steamclient64.dll"` (case-insensitive) → return
  `GetModuleHandleA("lcoverlay.dll")`, falling back to `"diversion.dll"` (legacy
  SteaMidra), falling back to the real lookup if neither is loaded.
- Anything else → chain straight through to the real `GetModuleHandleA`.

CR never knows it was redirected: from its perspective `steamclient64.dll`
simply *is* at lcoverlay's base, so its RVA arithmetic, RTTI scans and
`CCMInterface` traversal all walk the module where the live objects actually
are. In a non-LumaCore Steam neither diverted module exists, the thunk falls
through, and CR sees the real `steamclient64.dll` — which is correct there.

## 2. Arming the interception: the CR API, and why it changed

Older CR (**≤ 2.1.x**) armed its cloud-RPC vtable hooks *inside*
`CR_InitCloudSave` — one call did everything, so crbridge only had to make it.

CR (**≥ 2.2.5**) split that in two:

- `CR_InitCloudSave` now only wires up storage, the cloud provider and
  namespace-app detection.
- A separate export, **`CR_InstallVtableHooks()`**, patches Steam's transport
  vtable and is what actually makes CR intercept cloud RPCs. The host must call
  it explicitly.

crbridge resolves `CR_InstallVtableHooks` best-effort and calls it right after a
successful `CR_InitCloudSave`. If the export is absent (pre-2.2.5 CR) it's
skipped, because that CR already installed the hook itself.

This split is what silently broke crbridge when CR moved past v2.1.6: on a modern
CR everything looks healthy in the log — module resolved, API resolved, IAT
hooked, `CR_InitCloudSave` succeeded — but with no `CR_InstallVtableHooks` call
no `[VtHook]` line ever appears in CR's log, not a single cloud RPC is
intercepted, and no save syncs.

## Startup sequence

1. `version.dll` (the proxy in the Steam directory) loads at process startup and
   `LoadLibrary`s `crbridge.dll`.
2. `crbridge.dll`'s `InitThread` waits for `steamclient64.dll` (up to 10s) and
   best-effort waits for the LumaCore diversion module (5s).
3. Loads `cloud_redirect.dll` from the Steam directory (system search path as a
   fallback) and resolves `CR_InitCloudSave`, `CR_SetApps`, `CR_Shutdown` and
   `CR_InstallVtableHooks` via `GetProcAddress`. Aborts only if
   `CR_InitCloudSave` is missing; the rest are best-effort.
4. Installs the IAT hook on `cloud_redirect.dll`'s `GetModuleHandleA` import.
5. Calls `CR_InitCloudSave(steamPath, nullptr)`.
6. Calls `CR_InstallVtableHooks()` to arm the interception (see §2).

On `DLL_PROCESS_DETACH` crbridge calls `CR_Shutdown` (best-effort) so CR can
drain pending HTTP work and close sockets cleanly.

## Note on iteration 9 → 10

Iteration 9 used a Microsoft Detours hook on `BBuildAndAsyncSendFrame` to feed
CR's packet path. Iteration 10 dropped it for the IAT hook above — a direct
pointer write, no library dependency. That is also why the
`CR_InstallVtableHooks` call matters on modern CR: with the packet hook gone,
nothing else bootstraps CR's interception, so the host has to arm it explicitly.
