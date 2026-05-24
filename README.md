# crbridge

Standalone loader for [CloudRedirect](https://github.com/Selectively11/CloudRedirect) — runs without SteamTools.

## What this is

CloudRedirect enables Steam Cloud functionality for games that don't natively support it, by intercepting Steam's cloud-save RPC calls and redirecting them to Google Drive, OneDrive, or a local folder.

Upstream CloudRedirect is designed to be loaded by SteamTools, which has documented backdoor behavior (downloads and executes arbitrary DLLs from remote servers, harvests Steam credentials — see [SteamTools-Deep-Analyze](https://github.com/Hegxib/SteamTools-Deep-Analyze) for the technical writeup).

**crbridge replaces SteamTools** as CloudRedirect's host. It is designed to coexist with [SteaMidra (LumaCore)](https://github.com/Midrags/SFF), the open-source GPLv3 alternative people use for license unlocking. Together they let you have CR's cloud-save redirection without ever installing SteamTools.

This project does not modify CloudRedirect, SteaMidra, or LumaCore. It is a thin bridge that lets an unmodified CloudRedirect run in a SteamTools-free environment.

## Status

**Iteration 9 — end-to-end working** as of 2026-05-24. Verified with:

- Steam stable build `1779486452`
- CloudRedirect (md5 `0ee5a330…`, supports Steam builds `1779486452 / 1778281814 / 1778003620`)
- SteaMidra current release (LumaCore using `bin/lcoverlay.dll` as its diverted module)
- Test game: Balatro (`app=2379780`) added as a namespace app via SteaMidra Lua

Saves redirect to Google Drive via CR's local HTTP server. Steam UI reports clean "cloud updated" state with no sync errors.

See [Known limitations](#known-limitations) below for the things to watch out for.

## Requirements

- 64-bit Windows
- Steam stable channel within CR's whitelist (currently `1779486452`, `1778281814`, `1778003620` — this list ships hard-coded inside `cloud_redirect.dll` and changes on each CR release)
- SteaMidra / LumaCore already installed and working for license unlocking — otherwise crbridge runs fine but has nothing to bridge
- CloudRedirect's own dependencies: `cloud_redirect.dll` next to `steam.exe`, `cloud_redirect/config.json` configured, a cloud provider authenticated via CR's CLI

## Installation

1. Install [SteaMidra](https://github.com/Midrags/SFF) following its own instructions. Confirm it works standalone (a namespace app appears as owned in your Library).
2. Install [CloudRedirect](https://github.com/Selectively11/CloudRedirect):
   - Drop `cloud_redirect.dll` next to `steam.exe`
   - Create `cloud_redirect/config.json` with at minimum `{ "cloud_redirect": true }`
   - Authenticate a cloud provider via `CloudRedirect_CliMain` (CR's CLI)
3. Download the latest `crbridge` artifact zip from this repo's [Actions](../../actions) tab.
4. From the zip, copy `build/Release/crbridge.dll` **and** `build/proxy/Release/version.dll` into `C:\Program Files (x86)\Steam\` (next to `steam.exe`).
5. Restart Steam. Confirm in `%TEMP%\crbridge.log` that a line like `CRPatcher: PATCHED successfully` appears shortly after the first Cloud RPC fires (e.g., when launching a namespace-app game).

No manual DLL injection is needed. `version.dll` is a side-by-side proxy that Steam loads automatically at startup, which in turn loads `crbridge.dll`.

## How it works

CloudRedirect intercepts outgoing `Cloud.*` RPC packets, performs the redirect, and injects synthetic responses back into Steam so the client believes Valve approved the operation. Both halves require knowing where several Steam-internal functions and objects live in memory — under SteamTools, that knowledge is provided by SteamTools' `Payload.dll`.

crbridge does the same job differently:

1. A `version.dll` proxy in the Steam directory is loaded automatically by Steam at startup. It calls `LoadLibrary` on `crbridge.dll`.
2. `crbridge.dll` waits for LumaCore's `lcoverlay.dll` (or legacy `diversion.dll`) — the patched copy of `steamclient64.dll` where Steam's runtime actually executes — to be loaded.
3. It locates `BBuildAndAsyncSendFrame` inside that module via byte-pattern matching, and installs a Microsoft Detours hook (the same library LumaCore uses, so trampolines chain correctly).
4. On the first binary WebSocket frame, it walks Steam's internal user-handle registry to locate the running `CCMInterface` instance.
5. It writes that `CCMInterface` pointer plus three Steam helper functions (`wrapPacket`, `bRouteMsgToJob`, `releaseWrapped`) directly into CloudRedirect's `.data` section, then sets CR's init flag so CR skips its own (broken-in-this-environment) initialization.
6. Subsequent Cloud RPC packets are forwarded to `cloud_redirect.dll!CloudOnSendPkt`, which can now intercept and inject responses normally.

The whole bridge is ~700 lines of C++ in [src/](src/). The interesting logic is in [`src/cr_patcher.cpp`](src/cr_patcher.cpp).

## Known limitations

- **Hard-coded memory offsets.** The bridge relies on specific addresses inside `steamclient64.dll` (accessed via LumaCore's `lcoverlay.dll`) and inside `cloud_redirect.dll`. When Selectively11 ships a new CR build or Valve updates Steam, those addresses can shift and the bridge will silently stop working. The constants live in [`src/cr_patcher.cpp`](src/cr_patcher.cpp) with comments explaining what each one is and how it was derived. A refresh-procedure document is planned.
- **Load-order race with LumaCore.** crbridge polls up to 5 seconds for `lcoverlay.dll` to load before installing its hook. If LumaCore takes longer (rare), crbridge falls back to hooking the original `steamclient64.dll`, where Steam's runtime traffic doesn't actually go, and the bridge silently does nothing for that session. Restarting Steam usually resolves it.
- **No Steam-version drift warning.** If Steam updates beyond CR's hard-coded whitelist, CR aborts internally with `FATAL: Steam version mismatch` and crbridge has no visibility — your saves will silently stop redirecting. Planned: read the manifest at startup and surface this clearly in `crbridge.log`.
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

Microsoft Detours is fetched via CMake `FetchContent`; no separate setup needed.

## License

GPLv3 — see [LICENSE](LICENSE). Same license as LumaCore, which crbridge intentionally mirrors (some pattern-matching and Detours conventions are adapted from LumaCore's `PatternDb.h`).

## Credits

- [Selectively11/CloudRedirect](https://github.com/Selectively11/CloudRedirect) — the cloud redirection engine that does all the actual work
- [Midrags/SFF (SteaMidra + LumaCore)](https://github.com/Midrags/SFF) — the Steam injector ecosystem this is designed to coexist with; reference for hook patterns and the diversion technique
- [Hegxib/SteamTools-Deep-Analyze](https://github.com/Hegxib/SteamTools-Deep-Analyze) — static analysis of SteamTools' backdoor; motivation for this project
- [Microsoft Detours](https://github.com/microsoft/Detours) — function hooking library, MIT-licensed
