#!/usr/bin/env python3
"""
refresh_offsets.py — Extract crbridge's CR-side runtime constants from
a copy of cloud_redirect.dll, so they can be refreshed when CR releases
a new build (which it does ~every 1-2 days; the .data section shifts
with each one and crbridge's hardcoded RVAs would otherwise rot).

Usage:
    python tools/refresh_offsets.py <path-to-cloud_redirect.dll>

Output is a paste-ready block of `constexpr uintptr_t CR_RVA_*` lines
for cr_patcher.cpp, plus a comparison against the values currently
committed in the source tree if it can find them.

This script implements Category A: the 6 globals that live inside
cloud_redirect.dll's own .data section (steamclient64_base cache,
init flag byte, cmInterface, wrapPacket, bRouteMsgToJob, releaseWrapped).
Categories B and C (Steam-internal RVAs and struct offsets) are intended
to follow.

Method (high level):
  1. Parse cloud_redirect.dll as an x64 PE image.
  2. Find the anchor string "[INJECT] Cannot inject:" in .rdata.
  3. Find lea-xrefs to that string in .text — that's the log call site.
  4. Walk .pdata to locate the enclosing function's start RVA.
  5. Read `mov rXX, qword ptr [rip+disp32]` at the function prologue.
     These movs load the 4 INJECT globals; map registers to global
     names via the printf-format-order convention.
  6. Find steamclient64_base via the GetModuleHandleA("steamclient64.dll")
     pattern: lea rcx, [str]; ... ; mov [global], rax.
  7. Find init_flag via the `lock cmpxchg byte ptr [rip+disp32], cl`
     instruction (encoding: F0 0F B0 0D disp32).

Each step prints a clear failure if it can't find what it expects.
Pass --debug to see intermediate RVAs.

No external Python dependencies — pure stdlib, ~400 lines.
"""

import argparse
import re
import struct
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Minimal x64 PE parser (just enough to walk sections + pdata)
# ---------------------------------------------------------------------------

def parse_pe(data: bytes) -> dict:
    """Return dict with image_base + list of section dicts."""
    if data[:2] != b'MZ':
        raise ValueError('not a PE: missing MZ header')
    e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
    if data[e_lfanew:e_lfanew+4] != b'PE\x00\x00':
        raise ValueError('not a PE: missing PE signature')
    num_sections = struct.unpack_from('<H', data, e_lfanew + 6)[0]
    opt_hdr_size = struct.unpack_from('<H', data, e_lfanew + 0x14)[0]
    # Optional header layout (PE32+): magic(2) + ... + ImageBase(8) at offset 0x18.
    image_base = struct.unpack_from('<Q', data, e_lfanew + 0x18 + 0x18)[0]
    sect_off = e_lfanew + 0x18 + opt_hdr_size
    sections = []
    for i in range(num_sections):
        b = sect_off + i * 0x28
        sections.append({
            'name':    data[b:b+8].rstrip(b'\x00').decode('latin-1', errors='replace'),
            'vaddr':   struct.unpack_from('<I', data, b+12)[0],
            'vsize':   struct.unpack_from('<I', data, b+8)[0],
            'raw_off': struct.unpack_from('<I', data, b+20)[0],
            'raw_size':struct.unpack_from('<I', data, b+16)[0],
        })
    return {'image_base': image_base, 'sections': sections}


def section(pe: dict, name: str):
    for s in pe['sections']:
        if s['name'] == name:
            return s
    return None


def rva_to_fo(pe: dict, rva: int):
    for s in pe['sections']:
        if s['vaddr'] <= rva < s['vaddr'] + s['vsize']:
            return s['raw_off'] + (rva - s['vaddr'])
    return None


# ---------------------------------------------------------------------------
# Minimal x64 RIP-relative instruction scanner.
# We don't need a real disassembler — we only recognize the exact byte
# patterns we care about, each 6 or 7 bytes long.
#
# RIP-relative addressing uses ModRM byte 0bMM_REG_RM where MM=00 and RM=101.
# So ModRM-byte ∈ {0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D} maps to
# REG = rax..rdi (or r8..r15 depending on the REX.R bit in the REX prefix).
# ---------------------------------------------------------------------------

MODRM_TO_REG_LOW  = {0x05:'rax',0x0D:'rcx',0x15:'rdx',0x1D:'rbx',
                     0x25:'rsp',0x2D:'rbp',0x35:'rsi',0x3D:'rdi'}
MODRM_TO_REG_HIGH = {0x05:'r8', 0x0D:'r9', 0x15:'r10',0x1D:'r11',
                     0x25:'r12',0x2D:'r13',0x35:'r14',0x3D:'r15'}

def reg_from_rex_modrm(rex: int, modrm: int):
    if rex == 0x48:
        return MODRM_TO_REG_LOW.get(modrm)
    if rex == 0x4C:
        return MODRM_TO_REG_HIGH.get(modrm)
    return None


def iter_rip_insts(data: bytes, pe: dict, opcode: int):
    """Yield (inst_rva, reg_name, target_rva) for every 7-byte RIP-relative
    instruction in .text matching `<REX.W or REX.WR> opcode modrm disp32`."""
    text = section(pe, '.text')
    if not text:
        return
    base = text['raw_off']
    sz = min(text['raw_size'], len(data) - base)
    for i in range(sz - 6):
        rex = data[base + i]
        if rex not in (0x48, 0x4C):
            continue
        if data[base + i + 1] != opcode:
            continue
        modrm = data[base + i + 2]
        reg = reg_from_rex_modrm(rex, modrm)
        if reg is None:
            continue
        disp = struct.unpack_from('<i', data, base + i + 3)[0]
        inst_rva = text['vaddr'] + i
        yield inst_rva, reg, inst_rva + 7 + disp


# ---------------------------------------------------------------------------
# .pdata walker: each entry is 12 bytes (BeginAddress, EndAddress, UnwindInfo).
# ---------------------------------------------------------------------------

def function_containing(data: bytes, pe: dict, target_rva: int):
    pdata = section(pe, '.pdata')
    if not pdata:
        return None
    base = pdata['raw_off']
    sz = pdata['vsize']
    for i in range(0, sz, 12):
        beg = struct.unpack_from('<I', data, base + i)[0]
        end = struct.unpack_from('<I', data, base + i + 4)[0]
        if beg == 0 and end == 0:
            break
        if beg <= target_rva < end:
            return beg
    return None


# ---------------------------------------------------------------------------
# Anchor 1: find the 4 INJECT globals via the "Cannot inject" log call.
# ---------------------------------------------------------------------------

def find_inject_globals(data: bytes, pe: dict, debug: bool = False):
    """Returns a dict {wrapPacket, bRouteMsgToJob, releaseWrapped, cmInterface}
    of RVAs, or None if extraction fails."""
    rdata = section(pe, '.rdata')
    if not rdata:
        return None, 'no .rdata section'
    chunk = data[rdata['raw_off']:rdata['raw_off'] + rdata['raw_size']]

    # Try a couple of slightly-different anchor phrasings, just in case the
    # dev tweaks the message in a future release without major restructure.
    for anchor in (b'[INJECT] Cannot inject:', b'Cannot inject:'):
        pos = chunk.find(anchor)
        if pos >= 0:
            anchor_rva = rdata['vaddr'] + pos
            break
    else:
        return None, 'anchor string "[INJECT] Cannot inject:" not found in .rdata'

    if debug:
        print(f'[dbg] anchor string @ RVA 0x{anchor_rva:X}', file=sys.stderr)

    # Find lea-xrefs to the anchor.
    xrefs = [r for r in iter_rip_insts(data, pe, 0x8D) if r[2] == anchor_rva]
    if not xrefs:
        return None, f'no lea xref to anchor string @ 0x{anchor_rva:X}'
    if debug:
        for inst_rva, reg, _ in xrefs:
            print(f'[dbg] lea {reg}, [anchor] @ 0x{inst_rva:X}', file=sys.stderr)

    # For each xref, look up the enclosing function and read the first few
    # `mov rXX, [rip+disp32]` instructions of its prologue.
    for inst_rva, _, _ in xrefs:
        fn_start = function_containing(data, pe, inst_rva)
        if not fn_start:
            continue
        if debug:
            print(f'[dbg] xref 0x{inst_rva:X} is in function @ 0x{fn_start:X}',
                  file=sys.stderr)
        # Read up to 80 bytes of prologue. We use the same RIP-relative scanner
        # but restrict it to the function range.
        fo = rva_to_fo(pe, fn_start)
        if fo is None:
            continue
        prologue_movs = {}
        i = 0
        # Skip past the standard MS x64 prologue ops by stepping byte-by-byte;
        # we only collect mov-from-RIP. Stop after we find ALL 4 expected regs
        # or after 80 bytes.
        while i + 7 <= 80:
            here = fo + i
            rex = data[here]
            op  = data[here+1] if here + 1 < len(data) else 0
            if rex in (0x48, 0x4C) and op == 0x8B:
                modrm = data[here+2]
                reg = reg_from_rex_modrm(rex, modrm)
                if reg:
                    disp = struct.unpack_from('<i', data, here+3)[0]
                    inst_rva2 = fn_start + i
                    prologue_movs.setdefault(reg, inst_rva2 + 7 + disp)
                    i += 7
                    continue
            i += 1
            if all(r in prologue_movs for r in ('rdx','r8','r9','rax')):
                break
        if debug:
            for r, t in prologue_movs.items():
                print(f'[dbg]   mov {r}, [0x{t:X}]', file=sys.stderr)
        if all(r in prologue_movs for r in ('rdx','r8','r9','rax')):
            # The printf format string is
            #   "[INJECT] Cannot inject: wrapPacket=%p bRouteMsgToJob=%p "
            #   "releaseWrapped=%p cmInterface=%p"
            # With MS x64 ABI for printf-style calls:
            #   rcx = format ptr
            #   rdx = 1st %p -> wrapPacket
            #   r8  = 2nd %p -> bRouteMsgToJob
            #   r9  = 3rd %p -> releaseWrapped
            #   [rsp+0x20] = 4th %p (spilled from rax in the prologue)
            # That fixes the register-to-global mapping.
            return {
                'wrapPacket'    : prologue_movs['rdx'],
                'bRouteMsgToJob': prologue_movs['r8'],
                'releaseWrapped': prologue_movs['r9'],
                'cmInterface'   : prologue_movs['rax'],
            }, None
    return None, 'no candidate function had the expected 4-mov prologue'


# ---------------------------------------------------------------------------
# Anchor 2: steamclient64_base via GetModuleHandleA("steamclient64.dll")
# ---------------------------------------------------------------------------

def find_steamclient64_base_global(data: bytes, pe: dict, debug: bool = False):
    """The pattern in CR's source is:
        rcx = lea [rip+steamclient64.dll string]
        rax = call [iat:GetModuleHandleA]
        mov [rip+steamclient64_base_global], rax
    We anchor on the string, follow the lea, then walk forward looking for the
    mov [rip+disp32], rax encoded as `48 89 05 disp32` (7 bytes)."""
    rdata = section(pe, '.rdata')
    if not rdata:
        return None, 'no .rdata'
    chunk = data[rdata['raw_off']:rdata['raw_off'] + rdata['raw_size']]
    pos = chunk.find(b'steamclient64.dll\x00')
    if pos < 0:
        return None, '"steamclient64.dll" string not found in .rdata'
    str_rva = rdata['vaddr'] + pos
    if debug:
        print(f'[dbg] steamclient64.dll string @ 0x{str_rva:X}', file=sys.stderr)

    # Find every lea-xref to the string. We may have several (some used as
    # log-message arguments), so we have to filter for the GetModuleHandleA
    # usage specifically: it should be followed within a few instructions
    # by an indirect call and then a mov-to-global.
    xrefs = [r for r in iter_rip_insts(data, pe, 0x8D) if r[2] == str_rva]
    if not xrefs:
        return None, 'no lea xref to steamclient64.dll string'

    for lea_rva, lea_reg, _ in xrefs:
        if lea_reg != 'rcx':
            # GetModuleHandleA takes its argument in rcx — skip lea's into other regs.
            continue
        lea_fo = rva_to_fo(pe, lea_rva)
        if lea_fo is None:
            continue
        # Search up to 32 bytes after the lea for `48 89 05 disp32`
        # (mov qword ptr [rip+disp32], rax).
        for delta in range(7, 32):
            fo = lea_fo + delta
            if fo + 7 > len(data):
                break
            if (data[fo] == 0x48 and data[fo+1] == 0x89 and data[fo+2] == 0x05):
                disp = struct.unpack_from('<i', data, fo+3)[0]
                inst_rva = lea_rva + delta
                target = inst_rva + 7 + disp
                if debug:
                    print(f'[dbg] steamclient64_base global @ 0x{target:X} '
                          f'(mov [rip+disp32], rax after lea rcx,[str] @ 0x{lea_rva:X})',
                          file=sys.stderr)
                return target, None
    return None, 'lea rcx,[steamclient64.dll] found, but no mov [global],rax within 32 bytes'


# ---------------------------------------------------------------------------
# Anchor 3: init_flag byte via `lock cmpxchg byte ptr [rip+disp32], cl`
# Encoding: F0 0F B0 0D disp32  (8 bytes total).
# ---------------------------------------------------------------------------

def find_init_flag_global(data: bytes, pe: dict, debug: bool = False):
    text = section(pe, '.text')
    if not text:
        return None, 'no .text'
    base = text['raw_off']
    sz = min(text['raw_size'], len(data) - base)
    hits = []
    for i in range(sz - 7):
        if (data[base+i]   == 0xF0 and
            data[base+i+1] == 0x0F and
            data[base+i+2] == 0xB0 and
            data[base+i+3] == 0x0D):
            disp = struct.unpack_from('<i', data, base+i+4)[0]
            inst_rva = text['vaddr'] + i
            target = inst_rva + 8 + disp
            hits.append((inst_rva, target))
    if not hits:
        return None, 'no `lock cmpxchg byte ptr [rip+disp32], cl` instruction found'
    if debug:
        for inst, tgt in hits:
            print(f'[dbg] lock cmpxchg byte @ 0x{inst:X} -> 0x{tgt:X}', file=sys.stderr)
    # If there are multiple, pick the one whose target is closest to (but not
    # equal to) the cmInterface global we already located. The init_flag in
    # CR's source is exactly 9 bytes after cmInterface (it sits inside the
    # same once-init guard struct).
    if len(hits) == 1:
        return hits[0][1], None
    # Caller may pass the cmInterface RVA for tie-breaking via a side channel;
    # for now return all hits and let main() decide.
    return [t for _, t in hits], None


# ---------------------------------------------------------------------------
# Compare against currently-committed values, if available.
# ---------------------------------------------------------------------------

CURRENT_RVA_RE = re.compile(
    r'constexpr\s+uintptr_t\s+(CR_RVA_\w+)\s*=\s*0x([0-9A-Fa-f]+)\s*;'
)

def read_current_constants():
    """Best-effort read of the values currently in src/cr_patcher.cpp. If
    the file isn't where we expect, returns {}."""
    here = Path(__file__).resolve().parent
    candidate = here.parent / 'src' / 'cr_patcher.cpp'
    if not candidate.exists():
        return {}
    text = candidate.read_text(encoding='utf-8', errors='replace')
    out = {}
    for m in CURRENT_RVA_RE.finditer(text):
        out[m.group(1)] = int(m.group(2), 16)
    return out


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description='Extract CR-side data-section globals from cloud_redirect.dll',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument('dll_path', help='Path to cloud_redirect.dll')
    ap.add_argument('--debug', action='store_true',
                    help='Print intermediate RVAs and disasm bits to stderr')
    args = ap.parse_args()

    data = Path(args.dll_path).read_bytes()
    pe = parse_pe(data)

    inject, err = find_inject_globals(data, pe, debug=args.debug)
    if err:
        print(f'FATAL: could not extract INJECT globals: {err}', file=sys.stderr)
        sys.exit(1)

    sc_base, err = find_steamclient64_base_global(data, pe, debug=args.debug)
    if err:
        print(f'WARN:  could not extract steamclient64_base global: {err}', file=sys.stderr)

    flag, err = find_init_flag_global(data, pe, debug=args.debug)
    if err:
        print(f'WARN:  could not extract init_flag global: {err}', file=sys.stderr)
    if isinstance(flag, list):
        # Multiple cmpxchg sites; pick the one nearest cmInterface + 9 bytes.
        target = inject['cmInterface'] + 9
        flag = min(flag, key=lambda t: abs(t - target))

    # Print paste-ready block.
    extracted = {
        'CR_RVA_STEAMCLIENT_BASE': sc_base,
        'CR_RVA_CMINTERFACE'     : inject['cmInterface'],
        'CR_RVA_INIT_FLAG'       : flag,
        'CR_RVA_WRAP_PACKET'     : inject['wrapPacket'],
        'CR_RVA_BROUTE_MSG'      : inject['bRouteMsgToJob'],
        'CR_RVA_RELEASE_WRAPPED' : inject['releaseWrapped'],
    }
    current = read_current_constants()

    print('=== Extracted CR data-section constants ===')
    print()
    name_pad = max(len(k) for k in extracted)
    any_drift = False
    for name, val in extracted.items():
        if val is None:
            print(f'    // {name:<{name_pad}} = NOT FOUND')
            continue
        line = f'    constexpr uintptr_t {name:<{name_pad}} = 0x{val:X};'
        comment_parts = []
        if name == 'CR_RVA_INIT_FLAG':
            comment_parts.append('byte')
        cur = current.get(name)
        if cur is not None and cur != val:
            comment_parts.append(f'WAS 0x{cur:X} -- DRIFTED')
            any_drift = True
        if comment_parts:
            line += '  // ' + '; '.join(comment_parts)
        print(line)

    print()
    if current and not any_drift:
        print('All extracted values MATCH what is currently in src/cr_patcher.cpp.')
    elif current and any_drift:
        print('At least one value has DRIFTED. Update src/cr_patcher.cpp before recompiling.')
    else:
        print('(no current values to compare against — could not find src/cr_patcher.cpp)')


if __name__ == '__main__':
    main()
