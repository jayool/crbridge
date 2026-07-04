# crbridge

A 64-bit Windows DLL that boots [CloudRedirect](https://github.com/Selectively11/CloudRedirect) (CR) inside Steam **without SteamTools**, so its cloud-save redirection works alongside [SteaMidra / LumaCore](https://github.com/Midrags/SFF). Coexists with LumaCore without modifying or forking either project. How it works: [`docs/how-it-works.md`](docs/how-it-works.md).

> ⚠️ **Educational / research use only.** Use it with your own Steam account and content. Do not redistribute Valve binaries. This repo is only the bootstrap loader that lets an unmodified upstream CloudRedirect run without SteamTools.

## Installation

Steam **must be closed** when you copy the DLLs.

1. **LumaCore (via SteaMidra)** — install [SteaMidra](https://github.com/Midrags/SFF) and confirm it works standalone (a namespace app shows as owned in your Library).
2. **CloudRedirect** (v2.1.6+) — drop `cloud_redirect.dll` next to `steam.exe` and sign into a cloud provider via CR's app (it writes its own `config.json`).
3. **crbridge** — download `crbridge.zip` from the latest [Release](../../releases/latest); copy both `crbridge.dll` and `version.dll` into `C:\Program Files (x86)\Steam\`.
4. Start Steam and log in. Saves for your games redirect to the provider you configured.

**After a CloudRedirect update** — usually transparent. If a CR release binds `GetModuleHandleA` through an apiset crbridge doesn't cover, the log shows `IAT entry for GetModuleHandleA not found`; file an issue with the CR version.

### Tested platforms

- **Steam stable** on 64-bit Windows (10 / 11).
- **CloudRedirect v2.1.6+** (older builds lack the `CR_InitCloudSave` export).
- **SteaMidra current** (`lcoverlay.dll`); legacy `diversion.dll` builds work as a fallback.
- Other Steam channels and LumaCore-derived projects should work but are untested.

## What it does

crbridge is a host for CR's public C API. It plugs three pieces together:

| Piece | Role |
|---|---|
| **`version.dll` proxy** | Side-by-side DLL Steam auto-loads at startup. Its only job: `LoadLibrary` on `crbridge.dll`. |
| **IAT hook on `GetModuleHandleA`** | Redirects CR's `GetModuleHandleA("steamclient64.dll")` to LumaCore's diverted module (`lcoverlay.dll`, `diversion.dll` fallback) so CR resolves the live runtime, not the dormant vanilla copy. |
| **`CR_InitCloudSave` + `CR_InstallVtableHooks`** | Boots CR, then arms the cloud-RPC interception. CR ≥ 2.2.5 needs the second call to intercept anything; older CR self-installs and crbridge skips it. |

Division of labour — the three are orthogonal:

- **LumaCore** (via SteaMidra) → ownership / licensing layer (game appears as owned).
- **crbridge** → loads and drives CR; the IAT redirect.
- **CloudRedirect** → cloud-save RPC interception, redirected to Google Drive / OneDrive / local folder.

## Troubleshooting

Log: `%TEMP%\crbridge.log`. CR writes its own log next to `cloud_redirect.dll`.

A healthy startup ends with, in order:

```
CRLoader: cloud_redirect.dll loaded at …
CRIatHook: GetModuleHandleA hooked at IAT slot …
crbridge: CR_InitCloudSave succeeded.
crbridge: CR_InstallVtableHooks() -> hooks ACTIVE …
```

| Log line | Meaning / fix |
|---|---|
| `CR_InitCloudSave export not found …` | CR older than v2.1.6. Update CR. |
| `IAT entry for GetModuleHandleA not found …` | CR uses an import binding crbridge doesn't recognize (it tries `kernel32.dll` + two apisets). File an issue with a `dumpbin /imports cloud_redirect.dll`. |
| `no diversion module (lcoverlay.dll/diversion.dll) detected` | LumaCore didn't load within 5s (proxy chain inactive, load-order hijack, or slow load — a restart usually wins). With no LumaCore at all, crbridge proceeds with the redirect inert. |
| `CR_InitCloudSave returned false` | Usually Steam isn't logged in yet (`CCMInterface` only exists after login) — restart once logged in. Or the IAT hook didn't redirect (check the `CRIatHook` slot is non-null). |
| `CR_InstallVtableHooks() -> returned false` | CR couldn't hook the transport vtable — check `cloud_redirect.log` for the `[VtHook]` lines. |

The full failure-mode reasoning is in [`docs/how-it-works.md`](docs/how-it-works.md).

## How it works

CR expects to run inside SteamTools; crbridge replaces that host. It loads `cloud_redirect.dll`, redirects CR's Steam-module lookup to LumaCore's diverted image via an IAT hook so CR resolves the live runtime, then calls `CR_InitCloudSave` and `CR_InstallVtableHooks` to boot and arm CR. Full write-up — the LumaCore diversion problem, the IAT thunk, the CR API change that once broke this, and the startup sequence — in **[`docs/how-it-works.md`](docs/how-it-works.md)**.

## Build

Windows + Visual Studio 2022 + CMake 3.20+, no third-party C++ dependencies:

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Outputs `build/Release/crbridge.dll` and `build/proxy/Release/version.dll`. CI (`.github/workflows/build.yml`) builds every push and attaches `crbridge.zip` to a Release on `v*` tags.

## Credits / notes

- [Selectively11/CloudRedirect](https://github.com/Selectively11/CloudRedirect) — the cloud redirection engine that does the actual work.
- [Midrags/SFF (SteaMidra + LumaCore)](https://github.com/Midrags/SFF) — the injector ecosystem this coexists with.
- [Hegxib/SteamTools-Deep-Analyze](https://github.com/Hegxib/SteamTools-Deep-Analyze) — the SteamTools analysis that motivated a SteamTools-free loader.
- Research / educational. Use with your own Steam account and content. Do not redistribute Valve binaries.
