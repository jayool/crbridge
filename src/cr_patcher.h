#pragma once
#include <windows.h>

// CRPatcher — populates cloud_redirect.dll's INJECT globals from crbridge.
//
// CR's setter (`sub_180BAB70` inside cloud_redirect.dll) tries to resolve four
// pointers on first call: cmInterface, wrapPacket, bRouteMsgToJob, releaseWrapped.
// In a SteamTools-free environment the resolution fails silently (Steam's user
// registry lookup returns null), leaving all four globals at zero. The result
// in CR's log is: "[INJECT] Cannot inject: wrapPacket=0 ... cmInterface=0",
// every Cloud RPC falls through to Valve, and Steam shows a sync-error UI.
//
// This patcher resolves the same pointers ourselves (steamclient64.dll base +
// known RVAs, plus the cmInterface we extract from the BBuildAndAsyncSendFrame
// pObject) and writes them directly into CR's .data section, bypassing CR's
// own resolution path.
//
// Thread-safe; idempotent; safe to call from the hook on every packet.

namespace CRPatcher {

    // Attempts to patch CR's 6 INJECT-related globals. The candidate is the
    // pObject (this pointer) captured from a BBuildAndAsyncSendFrame call;
    // we verify its vtable against steamclient64.dll's CCMInterface::vftable
    // before trusting it. Returns true if globals are now populated (either
    // by this call or a previous successful one). Returns false if the
    // candidate's vtable did not match (caller may retry with a later
    // pObject) or if any module is not yet loaded.
    bool TryPatch(void* candidateCmInterface);

}
