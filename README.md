# crbridge

A 64-bit Windows DLL that bootstraps [CloudRedirect](https://github.com/Selectively11/CloudRedirect) inside Steam's process **without requiring SteamTools**, so you can use CR's cloud-save redirection alongside [SteaMidra (LumaCore)](https://github.com/Midrags/SFF). Designed to **coexist with LumaCore without modifying or forking either project**.

Upstream CR is designed to be loaded by SteamTools. Some users prefer not to rely on SteamTools (see [SteamTools-Deep-Analyze](https://github.com/Hegxib/SteamTools-Deep-Analyze) for one such analysis); crbridge is the alternative for them.

> ⚠️ **Educational / research use only.** Use it with your own Steam account
> and content. Do not redistribute Valve binaries. The author does not host or
> distribute any third-party content; this repo is only the bootstrap loader
> that lets an unmodified upstream CloudRedirect run without SteamTools.

## What it does

CR ≥ 2.1.6 exposes a public C API (`CR_InitCloudSave`, `CR_HandleCloudRpc`, `CR_AddApp` / `CR_RemoveApp` / `CR_SetApps`, `CR_Shutdown`) for hosts to drive it from outside. crbridge is one such host. It plugs three components together:

| Component | Role |
|---|---|
| **`version.dll` proxy** | Side-by-side DLL that Steam loads automatically at startup. Its only job is to `LoadLibrary` on `crbridge.dll`. |
| **IAT hook on `GetModuleHandleA`** | When CR's internal lookup calls `GetModuleHandleA("steamclient64.dll")`, we redirect it to LumaCore's diverted module (`lcoverlay.dll`, with `diversion.dll` fallback) so CR's RVA arithmetic resolves the live runtime instead of the dormant vanilla copy. |
| **`CR_InitCloudSave` + `CR_InstallVtableHooks`** | After the IAT hook is in place, crbridge calls CR's init function, then arms the cloud-RPC interception via `CR_InstallVtableHooks`. CR ≥ 2.2.5 no longer installs the hooks inside `CR_InitCloudSave`, so this second call is what makes CR actually intercept; on older CR (≤ 2.1.x) the export is absent and crbridge skips it (that CR self-installs). |

Division of labour:

- **LumaCore** (loaded via SteaMidra's proxy) → ownership / licensing layer (game appears as owned).
- **crbridge** (this project) → host loader for CR's cloud API; the IAT redirect.
- **CloudRedirect** (loaded via crbridge) → cloud-save RPC interception and redirection to Google Drive / OneDrive / local folder.

The three are orthogonal: crbridge does not touch what LumaCore or CR already handle; LumaCore does not know about CR; CR does not know about LumaCore (the IAT redirect makes the LumaCore environment transparent to it).

## Installation

Tested with Steam stable on 64-bit Windows. Steam **must be closed** when you copy the DLLs.

1. **LumaCore (via SteaMidra)**. Install [SteaMidra](https://github.com/Midrags/SFF) following its own instructions. Confirm it works standalone (a namespace app appears as owned in your Library).

2. **CloudRedirect** (v2.1.6 or newer). Drop `cloud_redirect.dll` next to `steam.exe` and authenticate a cloud provider via CR's app. The app writes its own `config.json` when you sign in — no manual editing required.

3. **crbridge**. Download `crbridge.zip` from the latest [Release](../../releases/latest):

   ```
   crbridge.zip
   ├── crbridge.dll
   └── version.dll
   ```

   Copy both DLLs into `C:\Program Files (x86)\Steam\` (next to `steam.exe`).

4. Start Steam and log in. Saves for your games redirect to the cloud provider you configured. Steam's UI shows clean "cloud updated" state.

### After a CloudRedirect update

Most updates are transparent to crbridge — the IAT hook only relies on CR continuing to use `GetModuleHandleA` to look up Steam, which is the standard Windows pattern.

The exception: if a CR release moves the import binding to a different DLL (e.g. an apiset crbridge doesn't yet cover), the hook installer will log `IAT entry for GetModuleHandleA not found` and abort. In that case, file an issue with the offending CR version so we can extend the lookup chain.

### Tested platforms

- **Steam stable** on 64-bit Windows (Windows 10 / 11).
- **CloudRedirect v2.1.6+** (older builds don't have the `CR_InitCloudSave` export crbridge depends on).
- **SteaMidra current release** using `bin/lcoverlay.dll` as its diverted module. Legacy SteaMidra builds using `bin/diversion.dll` are supported as a fallback path.
- Other Steam channels (beta / public-beta) and other LumaCore-derived projects should work but are untested.

## Troubleshooting

Log: `%TEMP%\crbridge.log` (open with `notepad %TEMP%\crbridge.log`).

### Verifying it's working

On a healthy startup the log contains, in order:

```
SteamLocator: steamclient64.dll loaded at …
SteamLocator: diversion module lcoverlay.dll loaded at …
CRLoader: cloud_redirect.dll loaded at …
CRLoader: resolved API: CR_InitCloudSave=… CR_SetApps=… CR_Shutdown=…
CRIatHook: GetModuleHandleA hooked at IAT slot …
crbridge: CR_InitCloudSave succeeded — CR is now active.
```

If any of these is missing, find the matching failure mode below.

### `CR_InitCloudSave export not found — is this cloud_redirect.dll v2.1.6 or newer?`

You have a CR build older than v2.1.6 (the version that introduced the public API). Update CR to v2.1.6 or newer.

### `IAT entry for GetModuleHandleA not found in cloud_redirect.dll's imports`

CR was built with an import binding crbridge doesn't recognize. The hook installer tries `kernel32.dll`, `api-ms-win-core-libraryloader-l1-2-0.dll`, and `api-ms-win-core-libraryloader-l1-1-0.dll` in that order. If your CR uses a different apiset, file an issue with the CR version and a `dumpbin /imports cloud_redirect.dll` output.

### `no diversion module (lcoverlay.dll/diversion.dll) detected`

LumaCore's diverted module didn't load within 5 seconds. Possible causes:

- SteaMidra isn't installed or its proxy chain didn't activate
- A version.dll-style proxy other than crbridge's is hijacking the load order
- LumaCore is slow to load on your machine — restarting Steam usually wins the race

If LumaCore isn't installed at all, crbridge proceeds anyway with the IAT redirect inert (CR sees vanilla steamclient64.dll, which is the correct behavior for non-LumaCore environments — but then there's nothing LumaCore-specific to bridge).

### `CR_InitCloudSave returned false`

CR's init function ran but reported failure. Common causes, ranked by frequency:

- Steam isn't logged in yet (CCMInterface only exists after login). Wait for login and restart Steam.
- The IAT hook didn't actually redirect. Check that the `CRIatHook` log line above shows a non-null slot.
- CR is older than v2.1.6 (see the export-not-found case above).
- LumaCore's diversion module loaded but doesn't have the runtime objects CR expects (rare — would indicate a SteaMidra change).

CR also writes its own log next to its DLL — check there for further detail.

## How it works (deep dive)

### The problem with CR ≥ 2.1.6 in a LumaCore environment

CR exposes `CR_InitCloudSave(steamPath, notifyCallback)` as the canonical entry point. CR's implementation does the heavy lifting internally: it calls `GetModuleHandleA("steamclient64.dll")` and walks Steam's `CSteamEngine` global to find the current `CCMInterface`. (On CR ≥ 2.2.5 the cloud-RPC vtable hooks are armed by a separate `CR_InstallVtableHooks` call rather than inside `CR_InitCloudSave` — see the sequence above.)

In a LumaCore-diverted Steam, the runtime code executes from `lcoverlay.dll` — a renamed copy of `steamclient64.dll` where LumaCore installs its own hooks. The vanilla `steamclient64.dll` is still loaded into the process (`GetModuleHandleA` returns its handle happily) but no live runtime objects exist there. CR walks the empty module, doesn't find CCMInterface, and `CR_InitCloudSave` returns `false`.

### The IAT redirect

crbridge intercepts CR's lookup by overwriting cloud_redirect.dll's Import Address Table entry for `GetModuleHandleA`. Our thunk:

- If the requested module name is `"steamclient64.dll"` (case-insensitive) → returns `GetModuleHandleA("lcoverlay.dll")`, falling back to `"diversion.dll"`, falling back to the real lookup if neither is loaded.
- Otherwise → chains through to the real `GetModuleHandleA` unchanged.

CR doesn't know it was redirected. From CR's perspective, `steamclient64.dll` simply is at lcoverlay's base. CR's internal RVA arithmetic, RTTI scans, and CCMInterface traversal all walk lcoverlay (where the live objects are) and `CR_InitCloudSave` succeeds.

### Sequence inside crbridge.dll

1. `version.dll` proxy in the Steam directory loads at process startup. It calls `LoadLibrary` on `crbridge.dll`.
2. `crbridge.dll`'s `InitThread` waits for `steamclient64.dll` to load (up to 10s) and best-effort waits for the LumaCore diversion module (5s).
3. Loads `cloud_redirect.dll` from the Steam directory; falls back to the system search path. Resolves `CR_InitCloudSave`, `CR_SetApps`, `CR_Shutdown` and `CR_InstallVtableHooks` via `GetProcAddress`. Aborts if `CR_InitCloudSave` is missing; the rest are best-effort.
4. Installs the IAT hook on cloud_redirect.dll's `GetModuleHandleA` import.
5. Calls `CR_InitCloudSave(steamPath, nullptr)`.
6. Calls `CR_InstallVtableHooks()` to arm the cloud-RPC interception. CR ≥ 2.2.5 moved this out of `CR_InitCloudSave`, so without this call CR initializes but never intercepts a single cloud RPC. If the export is absent (pre-2.2.5 CR), crbridge skips it — that CR still installs the hook inside `CR_InitCloudSave`.

On `DLL_PROCESS_DETACH` we call `CR_Shutdown` (best-effort) so CR can drain pending HTTP work and close sockets cleanly.

## Build infrastructure

CI (`.github/workflows/build.yml`) builds on every push to `main` and publishes a Release with `crbridge.zip` attached whenever a `v*` tag is pushed. The recommended path is to download the latest [Release](../../releases/latest); the per-push [Actions](../../actions) artifacts are also available but expire after 90 days.

To build locally on Windows with Visual Studio 2022 + CMake 3.20+:

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Outputs:

- `build/Release/crbridge.dll`
- `build/proxy/Release/version.dll`

No third-party C++ dependencies. The iteration 9 use of Microsoft Detours is gone (no function-level hooks left — the IAT hook is a direct pointer write).

## Credits / notes

- [Selectively11/CloudRedirect](https://github.com/Selectively11/CloudRedirect) — the cloud redirection engine that does all the actual work.
- [Midrags/SFF (SteaMidra + LumaCore)](https://github.com/Midrags/SFF) — the Steam injector ecosystem this is designed to coexist with.
- [Hegxib/SteamTools-Deep-Analyze](https://github.com/Hegxib/SteamTools-Deep-Analyze) — static analysis of SteamTools' backdoor; motivation for this project.
- Research / educational. Use with your own Steam account and content. Do not redistribute Valve binaries.
