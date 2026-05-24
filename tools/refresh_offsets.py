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
# Category B: SC_RVA_* — RVAs inside steamclient64.dll, encoded in CR's binary
# as immediates inside `lea reg, [base + imm32]` and `add reg, imm32`
# instructions. We extract them by anchoring on the CR-side globals we
# already located (Category A), which are STORED right after the lea/add.
# ---------------------------------------------------------------------------

def find_sc_rva_lea_before_global_store(data, pe, target_cr_global_rva):
    """For a CR-side global G at RVA `target_cr_global_rva`, locate the
    `mov qword ptr [rip+disp32], reg` that stores into it, then read the
    `lea reg, [base_reg + imm32]` exactly 7 bytes before. Returns imm32 or
    None.

    This is the encoding emitted by CR's setter for wrapPacket, bRouteMsg,
    and releaseWrapped:
        lea  rdx, [rcx + 0xD199E0]   ; 48 8D 91 disp32   (7 bytes)
        mov  [rip+disp32_global], rdx ; 48 89 15 disp32   (7 bytes)
    so the lea's imm32 IS the steamclient64.dll RVA we want.
    """
    text = section(pe, '.text')
    base = text['raw_off']
    sz = min(text['raw_size'], len(data) - base)
    for i in range(7, sz - 6):
        # Match `mov [rip+disp32], reg`:
        rex = data[base + i]
        if rex not in (0x48, 0x4C):
            continue
        if data[base + i + 1] != 0x89:
            continue
        modrm = data[base + i + 2]
        # mod=00, rm=101 (RIP-relative) → modrm & 0xC7 == 0x05.
        if (modrm & 0xC7) != 0x05:
            continue
        disp32 = struct.unpack_from('<i', data, base + i + 3)[0]
        inst_rva = text['vaddr'] + i
        if inst_rva + 7 + disp32 != target_cr_global_rva:
            continue
        # Found the mov-into-global. Inspect the previous 7 bytes for a lea
        # that targets the same register.
        prev_rex = data[base + i - 7]
        if prev_rex != rex:
            continue
        if data[base + i - 6] != 0x8D:
            continue
        prev_modrm = data[base + i - 5]
        # mod=10 (disp32), rm != 100 (no SIB).
        if (prev_modrm >> 6) != 2:
            continue
        if (prev_modrm & 7) == 4:
            continue
        # Source reg of mov must equal dest reg of lea (both reg field bits 3-5
        # with same REX.R encoded in the matching REX bytes — already enforced
        # by `rex == prev_rex`).
        if ((modrm >> 3) & 7) != ((prev_modrm >> 3) & 7):
            continue
        imm32 = struct.unpack_from('<I', data, base + i - 4)[0]
        return imm32
    return None


def find_sc_rva_ccmi_vtable(data, pe, sc_base_global_rva, debug=False):
    """Find `mov rdx, [rip+disp32_to_sc_base]; add rdx, imm32`. The imm32 is
    SC_RVA_CCMI_VTABLE. The same pattern can use other registers — we accept
    any reg-pair that matches itself.

    Encoding:
        48 8B XX disp32       ; mov rXX, [rip+disp32]    (7 bytes)
        48 81 YY imm32        ; add rXX, imm32           (7 bytes)
    where XX (modrm reg) and YY (modrm rm, since add r/m, imm has reg=000
    sub-opcode and rm = destination register) refer to the same register.
    """
    text = section(pe, '.text')
    base = text['raw_off']
    sz = min(text['raw_size'], len(data) - base)
    candidates = []
    for i in range(sz - 13):
        # mov part
        rex1 = data[base + i]
        if rex1 not in (0x48, 0x4C):
            continue
        if data[base + i + 1] != 0x8B:
            continue
        modrm1 = data[base + i + 2]
        if (modrm1 & 0xC7) != 0x05:
            continue
        disp32_1 = struct.unpack_from('<i', data, base + i + 3)[0]
        inst_rva = text['vaddr'] + i
        if inst_rva + 7 + disp32_1 != sc_base_global_rva:
            continue
        # add part, immediately after (7 bytes later)
        rex2 = data[base + i + 7]
        if rex2 not in (0x48, 0x4C):
            continue
        if data[base + i + 8] != 0x81:
            continue
        modrm2 = data[base + i + 9]
        # add r/m64, imm32 has reg-subop = 000, mod = 11 (reg-direct).
        if (modrm2 & 0xF8) != 0xC0:
            continue
        # Register of the add (modrm rm with REX.B from rex2) must equal
        # the register loaded by the mov (modrm reg with REX.R from rex1).
        mov_reg = ((rex1 & 4) >> 2) << 3 | ((modrm1 >> 3) & 7)
        add_reg = ((rex2 & 1)) << 3 | (modrm2 & 7)
        if mov_reg != add_reg:
            continue
        imm32 = struct.unpack_from('<I', data, base + i + 10)[0]
        candidates.append(imm32)
    if not candidates:
        return None, 'no `mov reg,[sc_base]; add reg, imm32` chain found'
    # All candidates should agree on the vtable RVA. If they don't, surface
    # that as a warning and pick the most-common one.
    from collections import Counter
    c = Counter(candidates)
    if debug:
        print(f'[dbg] CCMI_VTABLE candidates: {dict(c)}', file=sys.stderr)
    most_common, _ = c.most_common(1)[0]
    return most_common, None


def find_sc_rva_engine_ptr(data, pe, sc_base_global_rva, debug=False):
    """Find `mov reg, [rip+disp32_to_sc_base]; ...; mov reg2, [reg + imm32_big]`.
    The imm32 is SC_RVA_ENGINE_PTR_GLOBAL. We require imm32 > 0x100000 to
    filter out struct-field offsets, since 0x17A70E8 is a far-into-the-DLL
    data pointer.

    The intermediate `...` allows up to ~32 bytes of unrelated instructions
    (some setup, function calls, etc.) between the load of sc_base and the
    eventual dereference.
    """
    text = section(pe, '.text')
    base = text['raw_off']
    sz = min(text['raw_size'], len(data) - base)
    candidates = []
    for i in range(sz - 30):
        rex1 = data[base + i]
        if rex1 not in (0x48, 0x4C):
            continue
        if data[base + i + 1] != 0x8B:
            continue
        modrm1 = data[base + i + 2]
        if (modrm1 & 0xC7) != 0x05:
            continue
        disp32_1 = struct.unpack_from('<i', data, base + i + 3)[0]
        inst_rva = text['vaddr'] + i
        if inst_rva + 7 + disp32_1 != sc_base_global_rva:
            continue
        loaded_reg = ((rex1 & 4) >> 2) << 3 | ((modrm1 >> 3) & 7)
        # Scan the next ~32 bytes for `mov reg2, [reg_loaded + disp32]`.
        for j in range(7, 64):
            here = base + i + j
            if here + 7 > base + sz:
                break
            rex2 = data[here]
            if rex2 not in (0x48, 0x4C):
                continue
            if data[here + 1] != 0x8B:
                continue
            modrm2 = data[here + 2]
            # mod=10 (disp32), rm != 100, rm != 101 (those have special meaning).
            if (modrm2 >> 6) != 2:
                continue
            rm_bits = modrm2 & 7
            if rm_bits == 4 or rm_bits == 5:
                continue
            base_reg_for_deref = ((rex2 & 1)) << 3 | rm_bits
            if base_reg_for_deref != loaded_reg:
                continue
            disp32_2 = struct.unpack_from('<I', data, here + 3)[0]
            if disp32_2 < 0x100000:
                continue
            candidates.append(disp32_2)
            break  # one match per outer iteration is enough
    if not candidates:
        return None, 'no `mov reg,[sc_base]; ...; mov reg2,[reg + BIG_disp32]` chain found'
    from collections import Counter
    c = Counter(candidates)
    if debug:
        print(f'[dbg] ENGINE_PTR candidates: {dict(c)}', file=sys.stderr)
    most_common, _ = c.most_common(1)[0]
    return most_common, None


# ---------------------------------------------------------------------------
# Category C: ENGINE_OFF_* and USER_OFF_CCMINTERFACE — small struct-field
# offsets used by CR when traversing Steam's user registry.
# ---------------------------------------------------------------------------

def find_engine_offsets(data, pe, engine_ptr_rva, debug=False):
    """In the function that loads the engine pointer via
    `mov rcx, [rax + engine_ptr_rva]`, scan the following ~60 bytes for
    `mov reg, [rcx + disp32]` patterns. The three small disp32 values in
    the range 0xC00..0x1000 are USER_KEY (32-bit field), USER_ARRAY (64-bit
    pointer), USER_COUNT (32-bit field) — typically in that order.

    Heuristic for mapping:
      - The first 32-bit load        (`44 8B XX disp32`) = USER_KEY
      - The 64-bit load              (`4C 8B XX disp32`) = USER_ARRAY
      - The second 32-bit load       (`44 8B XX disp32`) = USER_COUNT
    """
    text = section(pe, '.text')
    base = text['raw_off']
    sz = min(text['raw_size'], len(data) - base)
    out = {'user_key': None, 'user_array': None, 'user_count': None}
    # Find the `mov reg, [reg + engine_ptr_rva]` site first.
    for i in range(sz - 6):
        rex = data[base + i]
        if rex not in (0x48, 0x4C):
            continue
        if data[base + i + 1] != 0x8B:
            continue
        modrm = data[base + i + 2]
        if (modrm >> 6) != 2:
            continue
        if (modrm & 7) == 4 or (modrm & 7) == 5:
            continue
        disp32 = struct.unpack_from('<I', data, base + i + 3)[0]
        if disp32 != engine_ptr_rva:
            continue
        # Now scan forward for up to ~80 bytes for accesses with small disp32.
        scan_start = base + i + 7
        scan_end = min(scan_start + 80, base + sz)
        loads32 = []
        loads64 = []
        j = scan_start
        while j + 6 < scan_end:
            r2 = data[j]
            op = data[j + 1]
            if (r2 in (0x44, 0x4C, 0x48) and op == 0x8B):
                m2 = data[j + 2]
                if (m2 >> 6) == 2:
                    rm = m2 & 7
                    if rm != 4 and rm != 5:
                        d32 = struct.unpack_from('<I', data, j + 3)[0]
                        if 0x800 <= d32 <= 0x1000:  # struct-field range
                            if r2 == 0x44:        # 32-bit, high reg
                                loads32.append(d32)
                            elif r2 == 0x48:      # 64-bit, low reg
                                loads64.append(d32)
                            elif r2 == 0x4C:      # 64-bit, high reg
                                loads64.append(d32)
                        j += 7
                        continue
            j += 1
        if debug:
            print(f'[dbg] engine-field 32-bit loads: {[f"0x{x:X}" for x in loads32]}',
                  file=sys.stderr)
            print(f'[dbg] engine-field 64-bit loads: {[f"0x{x:X}" for x in loads64]}',
                  file=sys.stderr)
        if len(loads32) >= 2 and len(loads64) >= 1:
            # Heuristic mapping by order of appearance.
            out['user_key']   = loads32[0]
            out['user_array'] = loads64[0]
            out['user_count'] = loads32[1]
            return out, None
    return out, 'could not locate the engine-field access sequence'


def find_user_off_ccminterface(data, pe, ccmi_vtable_rva, debug=False):
    """In the function that does the vtable verification, find
    `mov reg, [reg + disp8]` immediately before a `cmp reg, rdx` where rdx
    holds (sc_base + SC_RVA_CCMI_VTABLE).

    Easier path: anchor on the "[CCM] Vtable mismatch at CUser+72" message
    and look at the few instructions just before its lea-xref. That message
    is logged precisely when this `mov reg, [user + USER_OFF_CCMINTERFACE]`
    yielded a vtable other than the expected one.
    """
    rdata = section(pe, '.rdata')
    chunk = data[rdata['raw_off']:rdata['raw_off'] + rdata['raw_size']]
    pos = chunk.find(b'[CCM] Vtable mismatch at CUser+72:')
    if pos < 0:
        pos = chunk.find(b'Vtable mismatch at CUser+')
    if pos < 0:
        return None, 'CCM vtable-mismatch string not found in .rdata'
    str_rva = rdata['vaddr'] + pos
    # The string itself contains the offset literal "+72" (dec) = 0x48. That
    # is our answer in plain text. Verify by extracting the digits.
    end = chunk.find(b'\x00', pos)
    s = chunk[pos:end].decode('latin-1', errors='replace')
    m = re.search(r'CUser\+(\d+)', s)
    if m:
        val = int(m.group(1))
        if debug:
            print(f'[dbg] USER_OFF_CCMINTERFACE from log string "CUser+{m.group(1)}" = 0x{val:X}',
                  file=sys.stderr)
        return val, None
    return None, 'could not parse CUser+N from log string'


# ---------------------------------------------------------------------------
# Compare against currently-committed values, if available.
# ---------------------------------------------------------------------------

CURRENT_RVA_RE = re.compile(
    r'constexpr\s+uintptr_t\s+'
    r'(CR_RVA_\w+|SC_RVA_\w+|ENGINE_OFF_\w+|USER_OFF_\w+)'
    r'\s*=\s*0x([0-9A-Fa-f]+)\s*;'
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

    # === Category A: CR-side data globals ============================
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
        target = inject['cmInterface'] + 9
        flag = min(flag, key=lambda t: abs(t - target))

    extracted = {
        'CR_RVA_STEAMCLIENT_BASE': sc_base,
        'CR_RVA_CMINTERFACE'     : inject['cmInterface'],
        'CR_RVA_INIT_FLAG'       : flag,
        'CR_RVA_WRAP_PACKET'     : inject['wrapPacket'],
        'CR_RVA_BROUTE_MSG'      : inject['bRouteMsgToJob'],
        'CR_RVA_RELEASE_WRAPPED' : inject['releaseWrapped'],
    }

    # === Category B: RVAs inside steamclient64.dll, encoded in CR =====
    if extracted['CR_RVA_WRAP_PACKET']:
        extracted['SC_RVA_WRAP_PACKET'] = find_sc_rva_lea_before_global_store(
            data, pe, extracted['CR_RVA_WRAP_PACKET'])
    if extracted['CR_RVA_BROUTE_MSG']:
        extracted['SC_RVA_BROUTE_MSG'] = find_sc_rva_lea_before_global_store(
            data, pe, extracted['CR_RVA_BROUTE_MSG'])
    if extracted['CR_RVA_RELEASE_WRAPPED']:
        extracted['SC_RVA_RELEASE_WRAPPED'] = find_sc_rva_lea_before_global_store(
            data, pe, extracted['CR_RVA_RELEASE_WRAPPED'])

    if sc_base:
        vtable, err = find_sc_rva_ccmi_vtable(data, pe, sc_base, debug=args.debug)
        if err:
            print(f'WARN:  CCMI_VTABLE: {err}', file=sys.stderr)
        extracted['SC_RVA_CCMI_VTABLE'] = vtable

        eng, err = find_sc_rva_engine_ptr(data, pe, sc_base, debug=args.debug)
        if err:
            print(f'WARN:  ENGINE_PTR: {err}', file=sys.stderr)
        extracted['SC_RVA_ENGINE_PTR_GLOBAL'] = eng

        # === Category C: engine struct field offsets ==================
        if eng:
            engine_offs, err = find_engine_offsets(data, pe, eng, debug=args.debug)
            if err:
                print(f'WARN:  engine offsets: {err}', file=sys.stderr)
            extracted['ENGINE_OFF_USER_KEY']   = engine_offs.get('user_key')
            extracted['ENGINE_OFF_USER_ARRAY'] = engine_offs.get('user_array')
            extracted['ENGINE_OFF_USER_COUNT'] = engine_offs.get('user_count')

    # USER_OFF_CCMINTERFACE is independent of sc_base / Category B — it's
    # parsed directly from the "[CCM] Vtable mismatch at CUser+N" log string.
    uoff, err = find_user_off_ccminterface(
        data, pe, extracted.get('SC_RVA_CCMI_VTABLE'), debug=args.debug)
    if err:
        print(f'WARN:  USER_OFF_CCMINTERFACE: {err}', file=sys.stderr)
    extracted['USER_OFF_CCMINTERFACE'] = uoff

    current = read_current_constants()

    # Group + format the output.
    groups = [
        ('CR-side data-section globals (Category A)', [
            'CR_RVA_STEAMCLIENT_BASE',
            'CR_RVA_CMINTERFACE',
            'CR_RVA_INIT_FLAG',
            'CR_RVA_WRAP_PACKET',
            'CR_RVA_BROUTE_MSG',
            'CR_RVA_RELEASE_WRAPPED',
        ]),
        ('Steamclient64 RVAs (Category B)', [
            'SC_RVA_WRAP_PACKET',
            'SC_RVA_BROUTE_MSG',
            'SC_RVA_RELEASE_WRAPPED',
            'SC_RVA_CCMI_VTABLE',
            'SC_RVA_ENGINE_PTR_GLOBAL',
        ]),
        ('Steam struct field offsets (Category C)', [
            'ENGINE_OFF_USER_KEY',
            'ENGINE_OFF_USER_ARRAY',
            'ENGINE_OFF_USER_COUNT',
            'USER_OFF_CCMINTERFACE',
        ]),
    ]

    any_drift = False
    name_pad = max(len(n) for _, names in groups for n in names)
    for title, names in groups:
        print(f'=== {title} ===')
        print()
        for name in names:
            val = extracted.get(name)
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
        print('(no current values to compare against -- could not find src/cr_patcher.cpp)')


if __name__ == '__main__':
    main()
