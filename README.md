# crbridge

Standalone loader for [CloudRedirect](https://github.com/Selectively11/CloudRedirect) — runs without SteamTools.

## What this is

CloudRedirect enables Steam Cloud functionality for games that don't natively support it, by intercepting Steam's cloud save RPC calls and redirecting them to Google Drive, OneDrive, or a local folder.

Upstream CloudRedirect requires SteamTools as its DLL injection host. **crbridge replaces SteamTools** with an independent, transparent loader, so you can use CloudRedirect on Steam without installing the Chinese SteamTools payload (which has documented backdoor behavior — see [SteamTools-Deep-Analyze](https://github.com/Hegxib/SteamTools-Deep-Analyze)).

crbridge consists of:

- `crbridge.dll` — a small DLL injected into Steam that hooks the RPC packet sender and forwards each packet to `cloud_redirect.dll!CloudOnSendPkt`
- A launcher executable that injects the DLL into Steam (TODO, future iteration)

This project does not modify CloudRedirect, SteamTools, SteaMidra, or LumaCore. It is a thin bridge that lets unmodified CloudRedirect run in an unmodified Steam process.

## Status

Early development. Iteration 1: minimal DLL that proves it gets loaded into a host process.

## Building

CI builds on every push via GitHub Actions. Download the `crbridge.dll` artifact from the latest successful run under the **Actions** tab.

To build locally on Windows you need Visual Studio 2022 with the C++ workload and CMake 3.20+:
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

Output: `build/Release/crbridge.dll`.
## Testing (Iteration 1)
Inject `crbridge.dll` into a running process (Notepad works for a first smoke test, Steam for the real check) using any DLL injector — Process Hacker is free and clean. Confirm that `C:\crbridge.log` is created and contains a line like `crbridge loaded into PID 1234, host=...\steam.exe`.
## License
GPLv3 — see [LICENSE](LICENSE).
## Credits
- [Selectively11/CloudRedirect](https://github.com/Selectively11/CloudRedirect) — the actual cloud redirection engine
- [Midrags/SFF (SteaMidra)](https://github.com/Midrags/SFF) — the Steam injector ecosystem this is designed to coexist with
- [Hegxib/SteamTools-Deep-Analyze](https://github.com/Hegxib/SteamTools-Deep-Analyze) — analysis of SteamTools' backdoor, motivation for this project