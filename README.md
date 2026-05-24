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

crbridge also runs a startup compatibility check (`VersionCheck`) that reads Steam's build ID from its manifest, extracts CR's current Steam-version whitelist directly from `cloud_redirect.dll`'s `.rdata` at runtime, and logs a clear MATCH / NO MATCH verdict — so if Steam ever updates beyond what CR supports, the reason is visible in `crbridge.log` instead of being a silent failure.

See [Known limitations](#known-limitations) below for the things to watch out for.

## Requirements

- 64-bit Windows
- Steam stable channel within CR's whitelist (currently `1779486452`, `1778281814`, `1778003620` — this list ships hard-coded inside `cloud_redirect.dll` and changes on each CR release). crbridge reads CR's whitelist live from the binary on every startup, so you do **not** need to update crbridge when CR adds support for newer Steam builds.
- SteaMidra / LumaCore already installed and working for license unlocking — otherwise crbridge runs fine but has nothing to bridge
- CloudRedirect's own dependencies: `cloud_redirect.dll` next to `steam.exe`, `cloud_redirect/config.json` configured, a cloud provider authenticated via CR's CLI

## Installation

1. Install [SteaMidra](https://github.com/Midrags/SFF) following its own instructions. Confirm it works standalone (a namespace app appears as owned in your Library).
2. Install [CloudRedirect](https://github.com/Selectively11/CloudRedirect):
   - Drop `cloud_redirect.dll` next to `steam.exe`
   - Create `cloud_redirect/config.json` with at minimum `{ "cloud_redirect": true }`
   - Authenticate a cloud provider via `CloudRedirect_CliMain` (CR's CLI)
3. Download the latest `crbridge` artifact zip from this repo's [Actions](../../actions) tab. The zip contains exactly two files at its root:

   ```
   crbridge.zip
   ├── crbridge.dll
   └── version.dll
   ```

4. Extract both DLLs and copy them into `C:\Program Files (x86)\Steam\` (next to `steam.exe`). Steam must be fully closed.
5. Start Steam and open your Library. Confirm in `%TEMP%\crbridge.log` that you see, in order:
   - `VersionCheck: status = MATCH — Steam build NNNNNNN is supported.` (quick sanity check — confirms Steam version is compatible with your CR)
   - `FunctionHook: BBuildAndAsyncSendFrame hooked at ... CR forwarding ENABLED` (hook installed)
   - After launching a namespace-app game: `CRPatcher: PATCHED successfully ...` (CCMInterface located, CR's INJECT path armed)

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

- **Hard-coded memory offsets.** The bridge relies on 15 specific RVAs and struct offsets inside `steamclient64.dll` (accessed via LumaCore's `lcoverlay.dll`) and inside `cloud_redirect.dll`. When Selectively11 ships a CR build that restructures its data section, or Valve restructures Steam, those addresses can shift and the bridge will silently stop working. [`tools/refresh_offsets.py`](tools/refresh_offsets.py) automates the recovery — see [Refreshing offsets](#refreshing-offsets-after-cr-or-steam-updates) below. Note that this does **not** apply to CR's Steam-version whitelist, which is extracted live on every startup (see `src/version_check.cpp`).
- **Load-order race with LumaCore.** crbridge polls up to 5 seconds for `lcoverlay.dll` to load before installing its hook. If LumaCore takes longer (rare), crbridge falls back to hooking the original `steamclient64.dll`, where Steam's runtime traffic doesn't actually go, and the bridge silently does nothing for that session. Restarting Steam usually resolves it.
- **Steam beyond CR's whitelist still doesn't work.** crbridge now detects this case and logs `VersionCheck: status = NO MATCH` with a clear explanation, but it cannot make CR support a Steam build CR doesn't know about. When this happens, wait for a new CR release that adds the build, or roll Steam back to a supported one.
- **Single-account assumption.** Tested with one logged-in Steam user. Behavior with Family Sharing or fast user switching is unverified.

## Refreshing offsets after CR or Steam updates

CloudRedirect ships releases very frequently (≈ every 1-2 days; 27 releases over 45 days observed mid-2026), and its `.data` section moves with each one — empirically by 28 KB up to 600 KB between consecutive builds. When that happens, the 15 hard-coded constants in [`src/cr_patcher.cpp`](src/cr_patcher.cpp) become stale and crbridge stops working. Typical symptoms:

- `CRPatcher: vtable mismatch ...` repeated in `crbridge.log`, never `PATCHED successfully`
- `[INJECT] Cannot inject:` returns in `cloud_redirect.log`
- Steam UI shows the cloud-sync error again

[`tools/refresh_offsets.py`](tools/refresh_offsets.py) extracts all 15 constants directly from a fresh `cloud_redirect.dll` by anchoring on stable features (log format strings like `[INJECT] Cannot inject:` and `[CCM] Vtable mismatch at CUser+N`, the unique `lock cmpxchg byte` instruction, etc.) that survive across CR releases:

```sh
python tools/refresh_offsets.py "C:\Program Files (x86)\Steam\cloud_redirect.dll"
```

The script prints a paste-ready block of `constexpr uintptr_t` lines and compares them against the values currently in `src/cr_patcher.cpp`. If everything matches, the last line is `All extracted values MATCH what is currently in src/cr_patcher.cpp.` and there's nothing to do. If anything drifted, drifted lines are flagged inline:

```
constexpr uintptr_t CR_RVA_STEAMCLIENT_BASE  = 0x11FAB0;  // WAS 0x126C30 -- DRIFTED
constexpr uintptr_t CR_RVA_CMINTERFACE       = 0x11FAC8;  // WAS 0x126C48 -- DRIFTED
...
At least one value has DRIFTED. Update src/cr_patcher.cpp before recompiling.
```

Workflow when this happens:

1. Run the script against your installed `cloud_redirect.dll`.
2. For each line marked `DRIFTED`, replace the corresponding `constexpr uintptr_t` line in `src/cr_patcher.cpp` with the new value.
3. Commit and push — CI rebuilds and produces a fresh artifact.
4. Deploy the new `crbridge.dll` over the old one.

Whole cycle: well under a minute of attention, instead of the hour or two of manual `dumpbin /DISASM` it would take otherwise.

**Tool limitations** (will be reported as `// X = NOT FOUND` in the output, never silently guessed):

- Very old CR builds (pre-April 2026) may use different code patterns that the script's anchors don't match. For Category B / C entries, manual derivation may be needed in that case.
- If Selectively11 renames or removes the log format strings the script anchors on (e.g. `[INJECT] Cannot inject:`), the corresponding extractions fail. Should be rare since those strings exist for developer diagnostics.
- Pure stdlib — no `pip install` needed. Requires Python 3.6+.

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
