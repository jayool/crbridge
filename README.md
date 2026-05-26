# crbridge

Standalone loader that lets [CloudRedirect](https://github.com/Selectively11/CloudRedirect) run under [SteaMidra (LumaCore)](https://github.com/Midrags/SFF) — without SteamTools.

## What this is

CloudRedirect (CR) gives "lua" games Steam Cloud functionality by intercepting Steam's cloud-save RPCs and redirecting them to Google Drive, OneDrive, or a local folder.

Upstream CR is loaded by SteamTools, which has documented backdoor behavior (see [SteamTools-Deep-Analyze](https://github.com/Hegxib/SteamTools-Deep-Analyze)). **crbridge replaces SteamTools as CR's host** so you can use CR's cloud-save redirection alongside SteaMidra/LumaCore (the open-source GPLv3 unlocker) without ever installing SteamTools.

crbridge does not modify CloudRedirect or SteaMidra. It is a thin loader.

## How it works

The trick is that CR's `ost` branch already knows how to **self-initialize** against a diverted steamclient64.dll copy — it just looks for that copy under the name `diversion.dll`. SteaMidra/LumaCore uses the exact same diversion technique, but names its copy `bin\lcoverlay.dll`.

So crbridge only has to bridge that name gap:

1. A `version.dll` side-by-side proxy in the Steam folder is loaded automatically by Steam at startup. It LoadLibrarys `crbridge.dll`.
2. `crbridge.dll` waits for SteaMidra's diverted module (`lcoverlay.dll`, or legacy `diversion.dll`) to be mapped.
3. It installs a `GetModuleHandleA/W` hook (via Microsoft Detours) so any lookup for "diversion.dll" resolves to SteaMidra's `lcoverlay.dll`.
4. It LoadLibrarys an **unmodified** ost-branch `cloud_redirect.dll`. CR's own self-init thread then finds the diverted module via the alias, locates the CCMInterface, installs its RPC hook, and starts redirecting — all on its own.

That's it. crbridge never patches CR's binary or memory, so **CR auto-updates keep working**: a freshly updated `cloud_redirect.dll` still looks for `diversion.dll`, and the alias still redirects it.

## Requirements

- 64-bit Windows
- [SteaMidra](https://github.com/Midrags/SFF) installed and working (a namespace app shows as owned in your Library)
- An **ost-branch** `cloud_redirect.dll` from the [JanitorialMess/CloudRedirect releases](https://github.com/JanitorialMess/CloudRedirect/releases), dropped next to `steam.exe`
- CR's own setup: `cloud_redirect/config.json` configured, a cloud provider authenticated via CR's CLI
- A Steam stable build within CR's whitelist (crbridge logs a MATCH / NO MATCH verdict)

## Installation

1. Install SteaMidra and confirm it works standalone.
2. Install CloudRedirect (ost build):
   - Drop the ost-branch `cloud_redirect.dll` next to `steam.exe`
   - Create `cloud_redirect/config.json` (at minimum a `cloud_redirect: true` entry)
   - Authenticate a cloud provider via CR's CLI
3. Download the latest `crbridge` artifact from this repo's Actions tab. It contains two files at its root:

       crbridge.dll
       version.dll

4. Copy both DLLs into your Steam folder (next to `steam.exe`), with Steam fully closed.
5. Start Steam. In %TEMP%\crbridge.log you should see, in order:
   - HostAlias: "diversion.dll" aliased to diverted module @ ...
   - CRLoader: cloud_redirect.dll loaded at ...
   - VersionCheck: status = MATCH — Steam build NNNNNNN is supported.
   - then CR takes over (see cloud_redirect.log).

## Known limitations

- **No diverted module = nothing happens.** If `lcoverlay.dll`/`diversion.dll` isn't loaded within 15 s (SteaMidra not installed/running), crbridge logs that and exits without loading CR.
- **Steam beyond CR's whitelist still doesn't work.** crbridge surfaces this as `VersionCheck: status = NO MATCH`, but it cannot make CR support a Steam build CR doesn't know about. Wait for a new CR release or roll Steam back.
- **Single-account assumption.** Tested with one logged-in Steam user.

## Building from source

CI builds on every push via GitHub Actions (windows-latest); grab the `crbridge` artifact from the latest run. This is a Windows-only project and cannot be built on Linux/macOS.

To build locally you need Visual Studio 2022 (C++ workload) and CMake 3.20+:

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release

Outputs: build/Release/crbridge.dll and build/proxy/Release/version.dll. Microsoft Detours is fetched via CMake FetchContent.

## License

GPLv3 — see [LICENSE](LICENSE). Same license as LumaCore.

## Credits

- [Selectively11/CloudRedirect](https://github.com/Selectively11/CloudRedirect) — the cloud redirection engine
- [JanitorialMess/CloudRedirect (ost branch)](https://github.com/JanitorialMess/CloudRedirect/tree/ost) — the self-init host support crbridge relies on
- [Midrags/SFF (SteaMidra + LumaCore)](https://github.com/Midrags/SFF) — the Steam injector this coexists with
- [Hegxib/SteamTools-Deep-Analyze](https://github.com/Hegxib/SteamTools-Deep-Analyze) — motivation for this project
- [Microsoft Detours](https://github.com/microsoft/Detours) — function hooking library
