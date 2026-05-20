#include <windows.h>
#include <strsafe.h>

static HMODULE g_realVersion = nullptr;

// ── Forward stubs ──────────────────────────────────────────────────────
// Lazy-resolve cada símbolo del version.dll real la primera vez que se llama.
// Steam consume estas APIs vía import table; tenemos que reexportar todas.

#define PROXY(name, ret, params, args)                                  \
    extern "C" __declspec(dllexport) ret WINAPI name params {            \
        using FnType = ret (WINAPI*) params;                              \
        static FnType fn = reinterpret_cast<FnType>(                      \
            g_realVersion ? GetProcAddress(g_realVersion, #name) : nullptr); \
        if (!fn) { SetLastError(ERROR_PROC_NOT_FOUND); return (ret)0; }   \
        return fn args;                                                   \
    }

PROXY(GetFileVersionInfoA,        BOOL,  (LPCSTR  f, DWORD h, DWORD l, LPVOID d),              (f, h, l, d))
PROXY(GetFileVersionInfoW,        BOOL,  (LPCWSTR f, DWORD h, DWORD l, LPVOID d),              (f, h, l, d))
PROXY(GetFileVersionInfoExA,      BOOL,  (DWORD f1, LPCSTR  f, DWORD h, DWORD l, LPVOID d),    (f1, f, h, l, d))
PROXY(GetFileVersionInfoExW,      BOOL,  (DWORD f1, LPCWSTR f, DWORD h, DWORD l, LPVOID d),    (f1, f, h, l, d))
PROXY(GetFileVersionInfoSizeA,    DWORD, (LPCSTR  f, LPDWORD h),                                (f, h))
PROXY(GetFileVersionInfoSizeW,    DWORD, (LPCWSTR f, LPDWORD h),                                (f, h))
PROXY(GetFileVersionInfoSizeExA,  DWORD, (DWORD f1, LPCSTR  f, LPDWORD h),                     (f1, f, h))
PROXY(GetFileVersionInfoSizeExW,  DWORD, (DWORD f1, LPCWSTR f, LPDWORD h),                     (f1, f, h))
PROXY(VerFindFileA,               DWORD, (DWORD f, LPCSTR  fn, LPCSTR  wd, LPCSTR  ad, LPSTR  cd, PUINT cl, LPSTR  dd, PUINT dl), (f, fn, wd, ad, cd, cl, dd, dl))
PROXY(VerFindFileW,               DWORD, (DWORD f, LPCWSTR fn, LPCWSTR wd, LPCWSTR ad, LPWSTR cd, PUINT cl, LPWSTR dd, PUINT dl), (f, fn, wd, ad, cd, cl, dd, dl))
PROXY(VerInstallFileA,            DWORD, (DWORD f, LPCSTR  sf, LPCSTR  dfn, LPCSTR  sd, LPCSTR  dd, LPCSTR  cd, LPSTR  tf, PUINT tl), (f, sf, dfn, sd, dd, cd, tf, tl))
PROXY(VerInstallFileW,            DWORD, (DWORD f, LPCWSTR sf, LPCWSTR dfn, LPCWSTR sd, LPCWSTR dd, LPCWSTR cd, LPWSTR tf, PUINT tl), (f, sf, dfn, sd, dd, cd, tf, tl))
PROXY(VerLanguageNameA,           DWORD, (DWORD l, LPSTR  s, DWORD c),                          (l, s, c))
PROXY(VerLanguageNameW,           DWORD, (DWORD l, LPWSTR s, DWORD c),                          (l, s, c))
PROXY(VerQueryValueA,             BOOL,  (LPCVOID b, LPCSTR  sb, LPVOID* lp, PUINT pl),         (b, sb, lp, pl))
PROXY(VerQueryValueW,             BOOL,  (LPCVOID b, LPCWSTR sb, LPVOID* lp, PUINT pl),         (b, sb, lp, pl))

// ── Loader logic ───────────────────────────────────────────────────────
static bool IsSteamProcess() {
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return false;
    const wchar_t* name = wcsrchr(exePath, L'\\');
    name = name ? (name + 1) : exePath;
    return _wcsicmp(name, L"steam.exe") == 0;
}

static void LoadCrbridge(HMODULE hSelf) {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(hSelf, path, MAX_PATH) == 0) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    *(slash + 1) = L'\0';
    StringCchCatW(path, MAX_PATH, L"crbridge.dll");
    LoadLibraryW(path);  // si falla, el proxy sigue funcionando como simple forward
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);

        // 1. Cargar el version.dll real desde System32 para los forwards
        wchar_t sysPath[MAX_PATH];
        if (GetSystemDirectoryW(sysPath, MAX_PATH) == 0) return FALSE;
        StringCchCatW(sysPath, MAX_PATH, L"\\version.dll");
        g_realVersion = LoadLibraryW(sysPath);
        if (!g_realVersion) return FALSE;

        // 2. Solo cargar crbridge dentro del proceso de Steam, no en
        //    steamwebhelper.exe ni en steamservice.exe ni en otros hijos
        if (IsSteamProcess()) {
            LoadCrbridge(hMod);
        }
    }
    return TRUE;
}
