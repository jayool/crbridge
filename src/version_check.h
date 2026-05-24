#pragma once
#include <windows.h>

// VersionCheck — surfaces Steam-vs-CloudRedirect version compatibility early.
//
// CloudRedirect ships a hard-coded whitelist of Steam build IDs in its .rdata
// section. When Steam updates beyond that whitelist, CR writes a FATAL line
// to its own log and silently no-ops — and without this check crbridge has
// no visibility into that, so the user sees "saves don't redirect anymore"
// with no clue why.
//
// VersionCheck reads Steam's build ID from package/steam_client_win64.manifest,
// extracts CR's current whitelist at runtime by scanning cloud_redirect.dll's
// .rdata for a cluster of plausible Steam-build timestamps (anchored on
// build-time-known values, then expanded heuristically), and logs a clear
// MATCH / NO MATCH verdict to crbridge.log.
//
// Should be called after cloud_redirect.dll is loaded (so the scan can run),
// but before any heavy hook-install work (so the verdict reaches the log
// even if a later step crashes).

namespace VersionCheck {
    // Reads the manifest, extracts CR's whitelist, compares, logs. Idempotent;
    // safe to call multiple times. Failures are non-fatal — every problem is
    // surfaced via a log line and the bridge continues.
    void Run();
}
