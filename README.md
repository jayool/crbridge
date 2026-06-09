# crbridge

Standalone loader for [CloudRedirect](https://github.com/Selectively11/CloudRedirect) — runs without SteamTools.

## What this is

CloudRedirect (CR) enables Steam Cloud functionality for games that don't natively support it, by intercepting Steam's cloud-save RPC calls and redirecting them to Google Drive, OneDrive, or a local folder.

Upstream CR is designed to be loaded by SteamTools, which has documented backdoor behavior (downloads and executes arbitrary DLLs from remote servers, harvests Steam credentials — see [SteamTools-Deep-Analyze](https://github.com/Hegxib/SteamTools-Deep-Analyze) for the technical writeup).

**crbridge replaces SteamTools** as CR's host. It is designed to coexist with [SteaMidra (LumaCore)](https://github.com/Midrags/SFF), the open-source GPLv3 alternative people use for license unlocking. Together they let you have CR's cloud-save redirection without ever installing SteamTools.

This project does not modify CR, SteaMidra, or LumaCore. It is a thin bridge that lets an unmodified CR run in a SteamTools-free environment.

## Status

**Iteration 10 (refactor/iat-hook)** — written against CR v2.1.6+'s public API. Not yet tested end-to-end against current CR on a LumaCore-enabled Steam install. Once verified, this replaces the iteration 9 approach (data-section pokes) which is broken with CR ≥ 2.1.6 because the relevant data globals are now self-initialized internally and the `CloudOnSendPkt` export is gone.

The iteration 9 README and code live in `main` branch; this README/code lives in `refactor/iat-hook`. Merge happens after the user reports green logs from a real Steam + LumaCore setup.

## Requirements

- 64-bit Windows
- CloudRedirect **v2.1.6 or newer** (`CR_InitCloudSave`, `CR_SetApps`, `CR_Shutdown` exports must exist). The bridge logs a diagnostic and refuses to continue if it loads an older `cloud_redirect.dll`.
- SteaMidra / LumaCore installed and working for license unlocking. crbridge runs fine without it (the IAT hook simply falls through to vanilla steamclient64) but in that case CR's cloud redirect doesn't have anything LumaCore-specific to bridge.
- CR's own dependencies: `cloud_redirect.dll` next to `steam.exe`, `cloud_redirect/config.json` configured, a cloud provider authenticated via the CR Flatpak app or `CloudRedirect_CliMain`.

## Installation

1. Install [SteaMidra](https://github.com/Midrags/SFF) following its own instructions. Confirm it works standalone (a namespace app appears as owned in your Library).
2. Install CloudRedirect:
   - Drop `cloud_redirect.dll` (v2.1.6+) next to `steam.exe`.
   - Create `cloud_redirect/config.json` with at minimum `{ "cloud_redirect": true }`.
   - Authenticate a cloud provider via the CR app.
3. Download the latest `crbridge` artifact zip from this repo's [Actions](../../actions) tab. The zip contains:
   ```
   crbridge.zip
   ├── crbridge.dll
   └── version.dll
   ```
4. Extract both DLLs and copy them into `C:\Program Files (x86)\Steam\` (next to `steam.exe`). Steam must be fully closed.
5. Start Steam and log in. Confirm in `%TEMP%\crbridge.log` that you see, in order:
   - `SteamLocator: steamclient64.dll loaded at …`
   - `SteamLocator: diversion module lcoverlay.dll loaded at …` (if you're using LumaCore)
   - `CRLoader: cloud_redirect.dll loaded at …`
   - `CRLoader: resolved API: CR_InitCloudSave=… CR_SetApps=… CR_Shutdown=…`
   - `CRIatHook: GetModuleHandleA hooked at IAT slot …`
   - `crbridge: CR_InitCloudSave succeeded — CR is now active.`

`version.dll` is a side-by-side proxy that Steam loads automatically at startup, which in turn loads `crbridge.dll`. No manual DLL injection.

## How it works (iteration 10)

CR v2.1.6 introduced a public C API: `CR_InitCloudSave(steamPath, notify)` self-initializes everything — finds CCMInterface inside Steam's runtime module, installs its own vtable hooks on the cloud RPC dispatcher, etc. The host's job collapses to one call.

There's a snag in the LumaCore case: CR's lookup hardcodes `GetModuleHandleA("steamclient64.dll")`. In a LumaCore-diverted Steam, the runtime code executes from `lcoverlay.dll` (a renamed copy of `steamclient64.dll` where LumaCore installs its own intercepts). The dormant vanilla `steamclient64.dll` is still loaded — `GetModuleHandleA` happily returns its handle — but no live runtime objects exist there. CR walks that empty module, finds no CCMInterface, and `CR_InitCloudSave` returns false.

crbridge fixes this with a small **IAT hook**: it overwrites cloud_redirect.dll's Import Address Table entry for `GetModuleHandleA` to point at a thunk we control. The thunk:

- If the requested module is `"steamclient64.dll"` → returns the handle of `lcoverlay.dll` (falling back to legacy `diversion.dll`).
- Anything else → chains to the real `GetModuleHandleA`.

CR doesn't know it was redirected. From CR's perspective `steamclient64.dll` simply *is* at lcoverlay's base. CR's internal RVA arithmetic, RTTI scans, and CCMInterface traversal all walk lcoverlay (where the live objects are) and `CR_InitCloudSave` succeeds.

Sequence inside `crbridge.dll`:

1. `version.dll` proxy in the Steam directory loads at process startup. It calls `LoadLibrary` on `crbridge.dll`.
2. `crbridge.dll`'s `InitThread` waits for `steamclient64.dll` to load (up to 10 s) and best-effort waits for the LumaCore diversion module.
3. Loads `cloud_redirect.dll` and resolves `CR_InitCloudSave`, `CR_SetApps`, `CR_Shutdown`. Aborts with a diagnostic if CR is older than 2.1.6 (no `CR_InitCloudSave` export).
4. Logs a `VersionCheck` line comparing the running Steam build against CR's whitelist (purely diagnostic, doesn't gate anything).
5. Installs the IAT hook on `cloud_redirect.dll`'s `GetModuleHandleA` import (kernel32 or apiset variants).
6. Calls `CR_InitCloudSave(steamPath, nullptr)`. CR does the rest.

On `DLL_PROCESS_DETACH` we call `CR_Shutdown` (best-effort) so CR can drain pending HTTP work and close sockets cleanly.

Everything fits in ~350 lines of C++ in [`src/`](src/). The interesting parts are [`src/cr_iat_hook.cpp`](src/cr_iat_hook.cpp) (the redirect) and [`src/dllmain.cpp`](src/dllmain.cpp) (the orchestration).

## Why this replaces the old approach

Iteration 9 worked against CR ≤ 2.1.5 by writing six pointers directly into CR's `.data` section: CCMInterface, three Steam helper functions (`wrapPacket`, `bRouteMsgToJob`, `releaseWrapped`), the effective module base, and a "ready" flag. CR was passive — it consumed those pointers and called the helpers we provided.

Two things make that approach impossible against current CR:

1. The data slots still exist but CR's internal init now writes them itself. Any value the host writes is overwritten when `CR_InitCloudSave` runs.
2. The `CloudOnSendPkt` export — the entry point the iteration 9 hook handler forwarded packets to — was removed. There's no public function to call.

Iteration 10 stops pretending to be SteamTools and instead patches the one assumption CR makes that doesn't hold in LumaCore mode (the module to walk). Everything else — RPC interception, response injection, app-list management — is CR's job now, not ours.

A nice side-effect: the iteration 9 fragility around CR's data-section layout is gone. There is no `refresh_offsets.py` anymore because there are no offsets to refresh. The bridge is opaque to CR's release cadence as long as CR keeps using `GetModuleHandleA` for its module lookup (which it has done across at least v2.1.6 and v2.1.7).

## Known limitations

- **CR ≥ 2.1.6 required.** Older CR builds don't have the `CR_InitCloudSave` export. crbridge logs a clear diagnostic and aborts in that case.
- **Load-order race with LumaCore.** crbridge polls up to 5 s for `lcoverlay.dll` to load. If LumaCore takes longer, the IAT hook still installs but redirects `GetModuleHandleA("steamclient64.dll")` to the vanilla module (because lcoverlay isn't there yet). CR's lookup then walks empty memory and `CR_InitCloudSave` returns false. Restarting Steam usually wins the race.
- **Steam beyond CR's whitelist still doesn't work.** crbridge detects and logs this via `VersionCheck`, but it can't make CR support a Steam build CR doesn't know about. Wait for a newer CR release or roll Steam back.
- **Hook only covers `GetModuleHandleA`.** If a future CR build switches the runtime lookup to `GetModuleHandleW`, walks PEB module list manually, or uses some other mechanism, the IAT hook misses it and crbridge stops working. Diagnostic in the log will be `crbridge: CR_InitCloudSave returned false` with no IAT-related error — that's the smell.
- **Single-account assumption.** Tested with one logged-in Steam user. Behavior with Family Sharing or fast user switching is unverified.

## Building from source

CI builds on every push via GitHub Actions. The easiest path is to download the `crbridge` artifact from the latest successful run under the **Actions** tab.

To build locally on Windows you need Visual Studio 2022 with the C++ workload and CMake 3.20+:

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Outputs:

- `build/Release/crbridge.dll`
- `build/proxy/Release/version.dll`

No third-party dependencies — the iteration 9 use of Microsoft Detours is gone (no function hooks left to install).

## License

GPLv3 — see [LICENSE](LICENSE). Same license as LumaCore.

## Credits

- [Selectively11/CloudRedirect](https://github.com/Selectively11/CloudRedirect) — the cloud redirection engine that does all the actual work
- [Midrags/SFF (SteaMidra + LumaCore)](https://github.com/Midrags/SFF) — the Steam injector ecosystem this is designed to coexist with
- [Hegxib/SteamTools-Deep-Analyze](https://github.com/Hegxib/SteamTools-Deep-Analyze) — static analysis of SteamTools' backdoor; motivation for this project
