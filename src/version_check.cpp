#include "version_check.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>

namespace {

    // ---- Logging (local copy of the pattern used in other modules) ----------

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

    // ---- Steam manifest reader ---------------------------------------------

    // Reads {steam_dir}/package/steam_client_win64.manifest and extracts the
    // numeric value of the top-level "version" "NNNNN" pair. The manifest
    // is a VDF text file; we parse only the one field we need.
    bool ReadSteamBuildFromManifest(uint64_t* outBuild) {
        char exePath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) return false;
        std::string path(exePath);
        size_t lastSep = path.find_last_of("\\/");
        if (lastSep == std::string::npos) return false;
        std::string manifestPath = path.substr(0, lastSep);
        manifestPath += "\\package\\steam_client_win64.manifest";

        FILE* f = nullptr;
        if (fopen_s(&f, manifestPath.c_str(), "rb") != 0 || !f) return false;
        char buf[8192];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        if (n == 0) return false;
        buf[n] = 0;

        const char* p = strstr(buf, "\"version\"");
        if (!p) return false;
        p += 9;  // past "version"
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
        if (*p != '"') return false;
        ++p;
        uint64_t v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + static_cast<uint64_t>(*p - '0');
            ++p;
        }
        if (v == 0) return false;
        *outBuild = v;
        return true;
    }

    // ---- PE section walker -------------------------------------------------

    bool FindSection(HMODULE module, const char* name,
                     const uint8_t** outBase, size_t* outSize) {
        if (!module) return false;
        auto base = reinterpret_cast<const uint8_t*>(module);
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        auto section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            if (strncmp(reinterpret_cast<const char*>(section->Name), name, 8) == 0) {
                *outBase = base + section->VirtualAddress;
                *outSize = section->Misc.VirtualSize;
                return true;
            }
        }
        return false;
    }

    // ---- Whitelist extraction ---------------------------------------------

    // Build-time known versions. These are used as anchors to seed the .rdata
    // scan: we search for any of them, then expand the cluster outward. As
    // long as at least ONE of these still appears in a future CR release,
    // the dynamic scan finds the current whitelist regardless of additions.
    //
    // Also serves as the fallback whitelist if the scan fails entirely
    // (e.g., CR rewrites its data section). Last refreshed 2026-05-24 from
    // cloud_redirect.dll md5 0ee5a330df13c226f09e94fc39bf9089.
    constexpr uint64_t kKnownAnchors[] = {
        1779486452ULL,  // 2026-05-22
        1778281814ULL,  // 2026-05-08
        1778003620ULL,  // 2026-05-05
    };
    constexpr size_t kKnownAnchorCount = sizeof(kKnownAnchors) / sizeof(kKnownAnchors[0]);

    // Steam build IDs are Unix timestamps. We accept timestamps from 2020-01-01
    // forward, with a small future buffer for clock skew. Anything outside this
    // range is almost certainly noise (e.g., ASCII bytes that happen to form a
    // uint64_t in a numerically-plausible range).
    bool IsPlausibleSteamBuild(uint64_t v) {
        constexpr uint64_t kLowerBound = 1577836800ULL;  // 2020-01-01 UTC
        uint64_t upperBound = static_cast<uint64_t>(time(nullptr)) + 30ULL * 86400;
        return v >= kLowerBound && v <= upperBound;
    }

    // CR's whitelist tracks recent Steam builds — typically the few that match
    // CR's current RVA assumptions. Empirically these cluster within ~weeks of
    // each other. A 12-month range is generous but tight enough to reject
    // noise that happens to be a plausible-looking timestamp.
    constexpr uint64_t kClusterRangeMaxSeconds = 12ULL * 30 * 86400;

    // Search .rdata for any of our known anchors. Returns pointer to the 8-byte
    // value in the loaded image, or nullptr.
    const uint8_t* FindAnchor(const uint8_t* rdataBase, size_t rdataSize) {
        for (uint64_t anchor : kKnownAnchors) {
            uint8_t needle[8];
            memcpy(needle, &anchor, sizeof(needle));
            // 8-byte aligned scan, since the array is a uint64_t[].
            for (size_t i = 0; i + 8 <= rdataSize; i += 8) {
                if (memcmp(rdataBase + i, needle, 8) == 0) {
                    return rdataBase + i;
                }
            }
            // Some compilers may place the array at non-8-aligned offsets in
            // .rdata (unusual but possible). Fall back to a bytewise scan.
            for (size_t i = 0; i + 8 <= rdataSize; ++i) {
                if (memcmp(rdataBase + i, needle, 8) == 0) {
                    return rdataBase + i;
                }
            }
        }
        return nullptr;
    }

    // From the anchor position, expand the run of plausible Steam-build values
    // backwards and forwards, gated by both the plausibility filter and the
    // cluster-range filter (so noise that happens to look like a timestamp
    // but is far from the real cluster gets rejected).
    void ExpandCluster(const uint8_t* anchorPos,
                       const uint8_t* rdataBase, size_t rdataSize,
                       const uint8_t** outStart, const uint8_t** outEnd) {
        const uint8_t* start = anchorPos;
        const uint8_t* end   = anchorPos + 8;

        auto rangeWith = [&](uint64_t candidate) -> uint64_t {
            uint64_t minV = candidate, maxV = candidate;
            for (const uint8_t* p = start; p < end; p += 8) {
                uint64_t x;
                memcpy(&x, p, sizeof(x));
                if (x < minV) minV = x;
                if (x > maxV) maxV = x;
            }
            return maxV - minV;
        };

        // Expand backwards
        while (start - 8 >= rdataBase) {
            uint64_t v;
            memcpy(&v, start - 8, sizeof(v));
            if (!IsPlausibleSteamBuild(v)) break;
            if (rangeWith(v) > kClusterRangeMaxSeconds) break;
            start -= 8;
        }

        // Expand forwards
        while (end + 8 <= rdataBase + rdataSize) {
            uint64_t v;
            memcpy(&v, end, sizeof(v));
            if (!IsPlausibleSteamBuild(v)) break;
            if (rangeWith(v) > kClusterRangeMaxSeconds) break;
            end += 8;
        }

        *outStart = start;
        *outEnd   = end;
    }

    // Returns true if extraction succeeded; populates outVersions and outArrayAddr.
    bool ExtractWhitelistFromCR(std::vector<uint64_t>* outVersions, void** outArrayAddr) {
        HMODULE cr = GetModuleHandleA("cloud_redirect.dll");
        if (!cr) return false;

        const uint8_t* rdataBase = nullptr;
        size_t rdataSize = 0;
        if (!FindSection(cr, ".rdata", &rdataBase, &rdataSize)) return false;

        const uint8_t* anchor = FindAnchor(rdataBase, rdataSize);
        if (!anchor) return false;

        const uint8_t* clusterStart = nullptr;
        const uint8_t* clusterEnd   = nullptr;
        ExpandCluster(anchor, rdataBase, rdataSize, &clusterStart, &clusterEnd);

        constexpr size_t kMaxReasonable = 32;
        size_t count = static_cast<size_t>(clusterEnd - clusterStart) / 8;
        if (count == 0 || count > kMaxReasonable) return false;

        outVersions->clear();
        outVersions->reserve(count);
        for (const uint8_t* p = clusterStart; p < clusterEnd; p += 8) {
            uint64_t v;
            memcpy(&v, p, sizeof(v));
            outVersions->push_back(v);
        }
        if (outArrayAddr) *outArrayAddr = const_cast<uint8_t*>(clusterStart);
        return true;
    }
}

namespace VersionCheck {

void Run() {
    char buf[640];

    uint64_t steamBuild = 0;
    if (!ReadSteamBuildFromManifest(&steamBuild)) {
        LogLine("VersionCheck: could not read steam_client_win64.manifest "
                "(missing, unreadable, or no \"version\" field) — skipping check");
        return;
    }
    snprintf(buf, sizeof(buf),
        "VersionCheck: Steam build detected = %llu",
        static_cast<unsigned long long>(steamBuild));
    LogLine(buf);

    std::vector<uint64_t> whitelist;
    void* arrayAddr = nullptr;
    bool extracted = ExtractWhitelistFromCR(&whitelist, &arrayAddr);

    if (!extracted) {
        // Fallback: build-time anchors are also a usable whitelist, even if
        // they're a bit stale. Better than refusing to answer.
        whitelist.assign(kKnownAnchors, kKnownAnchors + kKnownAnchorCount);
        snprintf(buf, sizeof(buf),
            "VersionCheck: WARNING — could not extract whitelist from "
            "cloud_redirect.dll .rdata (anchor not found, or extraction "
            "filter rejected the cluster). Falling back to build-time "
            "snapshot of %zu version(s). The result may be stale.",
            whitelist.size());
        LogLine(buf);
    } else {
        snprintf(buf, sizeof(buf),
            "VersionCheck: extracted %zu-entry whitelist live from "
            "cloud_redirect.dll .rdata @ %p",
            whitelist.size(), arrayAddr);
        LogLine(buf);
    }

    for (size_t i = 0; i < whitelist.size(); ++i) {
        snprintf(buf, sizeof(buf),
            "VersionCheck:   whitelist[%zu] = %llu",
            i, static_cast<unsigned long long>(whitelist[i]));
        LogLine(buf);
    }

    bool inWhitelist = std::find(whitelist.begin(), whitelist.end(), steamBuild)
                       != whitelist.end();
    if (inWhitelist) {
        snprintf(buf, sizeof(buf),
            "VersionCheck: status = MATCH — Steam build %llu is supported. "
            "Cloud redirection should work normally.",
            static_cast<unsigned long long>(steamBuild));
    } else if (extracted) {
        snprintf(buf, sizeof(buf),
            "VersionCheck: status = NO MATCH — Steam build %llu is NOT in "
            "the whitelist extracted from cloud_redirect.dll. CR will abort "
            "internally (look for FATAL in cloud_redirect.log) and saves "
            "will NOT redirect for this session. Wait for a new CR release "
            "that adds support for this Steam build, or roll Steam back to "
            "a supported build.",
            static_cast<unsigned long long>(steamBuild));
    } else {
        snprintf(buf, sizeof(buf),
            "VersionCheck: status = NO MATCH (using stale fallback list) — "
            "Steam build %llu was not found in crbridge's build-time "
            "snapshot. CR may or may not actually support it; check "
            "cloud_redirect.log for the authoritative answer.",
            static_cast<unsigned long long>(steamBuild));
    }
    LogLine(buf);
}

}  // namespace VersionCheck
