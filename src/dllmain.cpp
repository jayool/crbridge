// crbridge — standalone loader for CloudRedirect (v2.1.6+ API).
//
// Iteration 10 — IAT-hook approach.
//
// We rely on CR's v2.1.6+ public API: CR_InitCloudSave does all the heavy
// lifting (finds CCMInterface, installs vtable hooks on Steam's RPC path)
// IF it can locate Steam's runtime module. CR's lookup calls
// GetModuleHandleA("steamclient64.dll") — which, in a LumaCore-diverted
// environment, points at the dormant vanilla copy where no runtime objects
// live. We fix that by IAT-hooking GetModuleHandleA inside cloud_redirect.dll
// and transparently returning lcoverlay.dll (or legacy diversion.dll) for
// that one string. CR then walks lcoverlay's image and finds everything.
//
// Net effect: no more .data pokes, no more offset tracking, no more
// refresh_offsets.py. The bridge is opaque to CR's release cadence as long
// as CR keeps using GetModuleHandleA for its module lookup.

#include <windows.h>
#include <cstdio>
#include <string>
#include "cr_loader.h"
#include "cr_iat_hook.h"
#include "steam_locator.h"
#include "version_check.h"

namespace {

void LogLine(const char* msg) {
    char tempDir[MAX_PATH] = {};
    if (GetTempPathA(MAX_PATH, tempDir) == 0) return;
    char logPath[MAX_PATH] = {};
    snprintf(logPath, MAX_PATH, "%scrbridge.log", tempDir);
    FILE* f = nullptr;
    fopen_s(&f, logPath, "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, msg);
        fclose(f);
    }
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

// Returns the directory containing steam.exe with a trailing backslash.
// CR_InitCloudSave's contract requires the trailing separator.
std::string GetSteamInstallDir() {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string path(exePath);
    size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return "";
    std::string dir = path.substr(0, pos);
    if (dir.empty() || dir.back() != '\\') dir.push_back('\\');
    return dir;
}

DWORD WINAPI InitThread(LPVOID) {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    char line[640];
    snprintf(line, sizeof(line),
        "crbridge: loaded into PID %lu, host=%s",
        GetCurrentProcessId(), exePath);
    LogLine(line);

    // 1. Wait for Steam's runtime to load. Without it, the IAT hook still
    //    installs cleanly but CR_InitCloudSave will find nothing because no
    //    runtime module exists yet.
    HMODULE sc = SteamLocator::WaitForSteamClient(10000);
    if (!sc) {
        LogLine("crbridge: steamclient64.dll didn't load in 10s, aborting");
        return 0;
    }

    // 2. Best-effort wait for the LumaCore diversion module. If neither
    //    lcoverlay nor diversion shows up we proceed anyway — the IAT
    //    hook's internal fallback chains to the real GetModuleHandleA,
    //    so vanilla setups (no LumaCore) still work correctly.
    const char* divName = nullptr;
    HMODULE diversion = SteamLocator::WaitForDiversion(5000, &divName);
    if (diversion) {
        snprintf(line, sizeof(line),
            "crbridge: LumaCore diversion detected: %s at %p — CR will be "
            "redirected to it via the IAT hook",
            divName, (void*)diversion);
        LogLine(line);
    } else {
        LogLine("crbridge: no LumaCore diversion module detected — CR will "
                "see vanilla steamclient64.dll (correct if you're not running LumaCore)");
    }

    // 3. Load cloud_redirect.dll and resolve the v2.1.6+ API.
    if (!CRLoader::TryLoad()) {
        LogLine("crbridge: failed to load cloud_redirect.dll or resolve its API, aborting");
        return 0;
    }

    // 4. Version check (diagnostic). Reads Steam's build id and CR's
    //    whitelist, logs MATCH / NO MATCH. Doesn't abort either way.
    VersionCheck::Run();

    // 5. Install the IAT hook BEFORE CR_InitCloudSave so the very first
    //    GetModuleHandleA("steamclient64.dll") inside CR hits the redirect
    //    and the module base CR caches in g_steamClientBase is the
    //    diversion module's, not the original's.
    if (!CRIatHook::Install(CRLoader::GetModule())) {
        LogLine("crbridge: IAT hook install failed — refusing to call CR_InitCloudSave "
                "because CR would look at vanilla steamclient64.dll and silently fail "
                "to find CCMInterface");
        return 0;
    }

    // 6. Initialize CR. From CR's perspective:
    //      GetModuleHandleA("steamclient64.dll") -> our hook -> lcoverlay
    //      g_steamClientBase = lcoverlay's base
    //      TryFindCCMInterface() walks lcoverlay -> finds the live CCMInterface
    //      InstallServiceMethodHook() installs vtable hooks on the cloud RPC path
    //    We have nothing else to do — CR runs on its own from here.
    auto init = CRLoader::GetInitCloudSave();
    if (!init) {
        // Should be impossible after TryLoad returned true, but guard for it.
        LogLine("crbridge: CR_InitCloudSave pointer is null, aborting");
        return 0;
    }

    std::string steamPath = GetSteamInstallDir();
    if (steamPath.empty()) {
        LogLine("crbridge: couldn't determine Steam install directory");
        return 0;
    }

    snprintf(line, sizeof(line),
        "crbridge: calling CR_InitCloudSave(steamPath=\"%s\", notify=null)",
        steamPath.c_str());
    LogLine(line);

    bool ok = init(steamPath.c_str(), nullptr);
    if (!ok) {
        LogLine("crbridge: CR_InitCloudSave returned false. Probable causes: "
                "(a) Steam isn't logged in (CCMInterface only exists after login); "
                "(b) the IAT hook didn't actually redirect (check the CRIatHook log line above); "
                "(c) CR is older than v2.1.6 and the API is shaped differently");
        return 0;
    }

    LogLine("crbridge: CR_InitCloudSave succeeded — CR is now active. "
            "Cloud RPCs for namespace apps will be redirected to your configured provider.");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        {
            HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
            if (h) CloseHandle(h);
        }
        break;
    case DLL_PROCESS_DETACH: {
        // Best-effort clean shutdown of CR (drain pending work, close
        // sockets, etc). Safe to skip if CR_Shutdown isn't exported.
        auto shutdown = CRLoader::GetShutdown();
        if (shutdown) shutdown();
        break;
    }
    }
    return TRUE;
}
