#!/usr/bin/env python3
"""Renut's renderer (app/src/renut_engine, banjo-nx's copy as futari-nx adapted it) for Guwange's Direct3D (XDK 2.0.11164):
its device offsets and Futari-specific guest addresses translated from Futari's XDK (2.0.9328) to Guwange's
(guwange-nx, 2026-10-21). The renderer's offsets are already Futari's (tools/renut_futari_layout.py, from N&B's); this
is the next step, Futari -> Guwange.

How the table was derived: the same functions of both XDKs side by side (tools/d3d_match.py and the pairing in
app/src/renut_engine/guwange_d3d_functions.txt; then every Futari function of its Direct3D range 0x82096B28-0x820AE000
paired with Guwange's by instruction shape, 227 unique matches as anchors and the rest by their neighbours), and every
aligned load, store and addi of the pairs compared: 342 distinct displacements of 0x2800 and up, and below 0x2800 all
equal but the import slots and lis-built globals. Guwange's D3DDevice is Futari's with ONE word inserted, at Guwange
+0x359C, and the shift taken back by the alignment padding before the 8-byte-aligned qword at +0x5530:

  Futari offset         Guwange = Futari +   pinned by (Futari sub -> Guwange sub)
  < 0x359C              0                    the public device (0x0000-0x2B00: 133 offsets equal, e.g. m_pRing +0x30,
                                             constants +0x480/+0x1920, fence tag +0x2A9C, flags +0x2ABC-0x2ABF in every
                                             setter); vertex declaration +0x2ED8 (SetVertexDeclaration 8209AF10 ->
                                             8225E238), index buffer +0x3144 (SetIndices 820992F8 -> 82257D68), render
                                             targets +0x3148.. and depth +0x3158 (SetRenderTarget 82099AD8 -> 822584B0,
                                             SetDepthStencilSurface 82099E28 -> 82258800, both instruction for
                                             instruction), stride bytes +0x31A0 (SetStreamSource), viewport
                                             +0x3218-0x3224 (D3D_SetViewport), scissor +0x3234 (SetScissorRect), PS / VS
                                             +0x3244 / +0x3248 (SetPixelShader / SetVertexShader), predication
                                             +0x3254-0x325C (SetPredication, identical), tiling surfaces +0x3268, tile
                                             count / rects / origins +0x327C / +0x3280 / +0x3370 and extent
                                             +0x342C-0x3434 (Guwange's own BeginTiling 8225B918; Futari links none),
                                             tiling clear colour +0x3440-0x3454 (BeginTiling / EndTiling 8225BC88), the
                                             in-KickOff word +0x3460 (KickOff 820A8FC0 -> 8226A6B0), +0x3500 / +0x350C /
                                             +0x3510 (KickOffSegment 820A8E38 -> 8226A528), export words +0x3590-0x3598
                                             (EndExport 820A3C38 -> 82254960); the last equal word is +0x3598.
  0x359C - 0x552B       +4                   the inserted word: the hardware init (820AB6D8 -> 8226CD58) stores 0 at
                                             Guwange +13724 after InitializeApiState, and Swap's new last call 82275E60
                                             calls the function pointer there when it is set (nothing else writes it).
                                             Command buffer 13724 -> 13728 (AllocateInlineData 820A8138 -> 822697C0,
                                             KickOffSegment); the ring words 13732 / 13736 -> 13736 / 13740 (SetIndices,
                                             SetStreamSource, SetTexture, SetPixelShader, SetVertexShader, EndExport);
                                             present parameters +0x35B8.. (Swap addi 13752 -> 13756, the front format
                                             13816 -> 13820, the Swap's parameter block 13848 -> 13852); present interval
                                             13804 -> 13808 (InitializeApiState 820AAF68 -> 8226C5E8,
                                             SynchronizeToPresentationInterval 820A4FD8 -> 82265208); scaler parameters
                                             13932 -> 13936 (Swap); the GPR table 13988-14004 (SetShaderGPRAllocation
                                             8209B098 -> 8225E3C0); automatic surfaces 15024-15048 (UnsetAll,
                                             InitializePresentationParameters 820AB1D0 -> 8226C850,
                                             SetRingBufferParameters); ring allocator 15104-15148 (RingBufferDeviceAllocate
                                             820A7F08 -> 82269590, SetRingBufferParameters 820A92E0 -> 8226AA28); critical
                                             sections 15152 / 15180 -> 15156 / 15184 (hardware init); gamma table 15212 ->
                                             15216 (Swap); vertical blank and flip queue 16748-16784 (the vblank handler
                                             820A4C08 -> 82264E88, SetVerticalBlankCallback 820A4F78 -> 822651F8); swap
                                             count 16768 -> 16772 (Swap); 16908-16936 (side buffer 16936 -> 16940: the
                                             device init 820AB668 -> 8226CCE8); 21740-21800 (minus-one words 21768 / 21772
                                             -> 21772 / 21776 and the performance frequency / period 21796 / 21800 ->
                                             21800 / 21804: the hardware init; 21740 -> 21744 in Swap and the fence code
                                             820A8638 -> 82269D08); the last +4 word is Futari's +0x5528 (-> 0x552C).
  0x552C                (no field)           Futari's padding before the qword +0x5530: no Futari code touches +21804.
  >= 0x5530             0                    ld / std +0x5530 / +0x5538 (820A44B0 -> 82258B10), +0x5540-0x5594
                                             (820AC838 / 820ACC88 -> 8226DD30 / 8226E180), +0x5C00-0x5C28, gamma pending
                                             +0x5E64 / +0x5E68, behaviour flags +0x5E80 (Direct3D_CreateDevice 820A4AC0 ->
                                             822591F0, identical: 24,320 bytes allocated, +24192 stored).

Every renderer offset this tool moves has Guwange accesses at its new value (python tools/renut_guwange_layout.py
--pins lists them, from app/generated/default). Every displacement pair that disagrees with the table, checked one by
one, is a load off a lis-built global or an instruction of a mis-paired small function. The gaps without
accesses in either XDK (0x36B4-0x3AB0, 0x3B50-0x416C, 0x4228-0x54EC) could in principle hide an insertion and a removal
that cancel; the renderer uses no offset inside them except array bases pinned at both ends.

Allocation: 0x5F00 bytes (24,320, Direct3D_CreateDevice 822591F0's li r3,24320), as Futari's: kDeviceAllocSize stays.

Globals (guest addresses the renderer reads or calls; Futari -> Guwange, each from the paired function):
  kGlobalQwordVa            0x821E1AF8 -> 0x82639340  the device init 8226CCE8 (lis 0x8264, ld -27840) copies it into
                                                      +10880; its bytes are 0002 0000 2B9C 4000 (Futari: 2470, 9328)
  kVdGlobalDeviceImportSlot 0x820005F8 -> 0x82000874  the hardware init 8226CD58 / BlockOnFence / IsFencePending /
                                                      Destroy (lis 0x8200, lwz 2164; 2168, VdGlobalXamDevice, when
                                                      KeGetCurrentProcessType() is 2); both images hold FFCAE000 there
  kRenderStateTable         0x821E14F0 -> 0x82638D28  InitializeApiState 8226C5E8 (lis 0x8264, addi -29400)
  kSamplerStateTable        0x821E19B0 -> 0x826391E8  InitializeApiState (addi -28184); 101 / 20 entries as Futari's
                                                      (same loop bounds), every default word equal
  kFormatTable (2 files)    0x82003430 -> 0x82008620  FindTextureSize / GetMipTailLevelOffsetCoords / Lock2DSurface
                                                      (lis 0x8201, addi -31200); its 128 bytes are Futari's
  kFullViewport             0x820031D0 -> 0x820086A0  D3D_SetSurfaceInfo 82258420 (addi -31072); the same 24 bytes
  kHiZOwnerVa               0x821ED330 -> 0x826974A4  CreateSurface 8225B298 and Destroy 82254BC0 (lis 0x8269, 29860)
  kGameNoLargePagesVa       0x821ED308 -> 0x8269748C  XPhysicalAlloc 82089ED8 -> 82253340 (identical but this load)
  kVblankHandler            0x820A4C08 -> 0x82264E88  the handler (70 instructions, Futari's 69: the flip queue's index
  kVblankHandlerWords       69 -> 70                  is computed one instruction longer); kVblankHandlerHash is the
  kVblankHandlerHash        -> 0x7D733D9A9AF2BAAE     FNV-1a of those 70 words in out/default_image.bin
  GuestGetCurrentThreadId   sub_8208DA10 -> sub_82252CF0  the device init's call (3 instructions, identical)
  DefaultXMemFree           sub_82088CE0 -> sub_822522C8  CreateDevice's failure path (6 instructions, identical)
Not translated, reported: kDeviceCapsVa 0x82003800 (Futari's D3DCAPS9 for GetDeviceCaps, which Guwange does not link:
its hook gets no site), and comments that quote Futari or N&B offsets.

What the N&B -> Futari table (tools/renut_futari_layout.py) gets wrong, found by pairing N&B's functions with Guwange's
for the ones Futari lacks (offsets are equal between Futari and Guwange there): N&B 0x2AC4-0x2AFF moved +4 (not 0:
SetRingBufferParameters 10952 / 10984 / 10988, CBlocker_Check 11004), and N&B 0x33B4-0x343C is not one +0xC8 step
(0x33B4 / 0x33C0 +0xB0, 0x33C8 / 0x33CC +0xB4 in DrawVertices / DrawIndexedVertices, 0x3424 +0xC4, 0x3438 +0xC8). The
renderer uses no offset in either range, so nothing follows from it here.

  python tools/renut_guwange_layout.py            dry run: every line it would change (file:line, old -> new)
  python tools/renut_guwange_layout.py --pins     also: the Guwange accesses at each new offset (app/generated/default)
  python tools/renut_guwange_layout.py --write    apply (idempotent: a translated file carries a marker)
"""
import argparse
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.path.join(ROOT, 'app', 'src', 'renut_engine')
GEN = os.path.join(ROOT, 'app', 'generated', 'default')
MARK = '// guwange-nx: device offsets translated to Guwange\'s XDK (tools/renut_guwange_layout.py)'
FUTARI_MARK = '// futari-nx: device offsets translated to Futari\'s XDK (tools/renut_futari_layout.py)'
SKIP_FILES = {'futari_renut_hooks.h', 'futari_renut_core_hooks.cpp'}  # generated by renut_futari_hooks.py

INSERT = 0x359C  # Guwange's inserted word: Futari offsets from here move
END = 0x552C     # ... up to here (0x552C itself is Futari's padding: no field)


def shift(x):
    return 4 if INSERT <= x < END else 0


def fmt(old_text, value):
    return ('0x%X' % value) if old_text.lower().startswith('0x') else str(value)


def translate(value_text):
    v = int(value_text, 16) if value_text.lower().startswith('0x') else int(value_text)
    return fmt(value_text, v + shift(v))


def value_of(text):
    return int(text, 16) if text.lower().startswith('0x') else int(text)


# Guest addresses: constant name -> (Futari's value, Guwange's value).
GLOBALS = {
    'kGlobalQwordVa': ('0x821E1AF8', '0x82639340'),
    'kVdGlobalDeviceImportSlot': ('0x820005F8', '0x82000874'),
    'kRenderStateTable': ('0x821E14F0', '0x82638D28'),
    'kSamplerStateTable': ('0x821E19B0', '0x826391E8'),
    'kFormatTable': ('0x82003430', '0x82008620'),
    'kFullViewport': ('0x820031D0', '0x820086A0'),
    'kHiZOwnerVa': ('0x821ED330', '0x826974A4'),
    'kGameNoLargePagesVa': ('0x821ED308', '0x8269748C'),
    'kVblankHandler': ('0x820A4C08', '0x82264E88'),
    'kVblankHandlerWords': ('69', '70'),
    'kVblankHandlerHash': ('0x49BA3A1B1EAA1B6E', '0x7D733D9A9AF2BAAE'),
}
# Not Guwange's, left as they are (reported).
KEEP = {'kDeviceCapsVa': 'Futari\'s D3DCAPS9 (0x82003800): Guwange links no GetDeviceCaps, so its hook gets no site'}
# Guest functions the renderer imports by address.
IMPORTS = [('__imp__sub_8208DA10', '__imp__sub_82252CF0', 'GetCurrentThreadId'),
           ('__imp__sub_82088CE0', '__imp__sub_822522C8', 'XMemFree')]

NUM = r'(0x[0-9A-Fa-f]+|\d{4,5})'
CONST = re.compile(r'^(\s*(?:static\s+)?constexpr\s+(?:u8|u16|u32|u64|uint32_t|uint64_t)\s+(\w+)\s*=\s*)'
                   r'(0x[0-9A-Fa-f]+|\d+)((?:u|U|ull|ULL)?\s*;.*)$')
CONST_ARRAY = re.compile(r'^(\s*constexpr\s+u32\s+(kDeviceOffsetsMinusOne)\[\]\s*=\s*\{)([^}]*)(\};.*)$')
# Device-offset contexts (in the code part of a line).
CONTEXTS = [
    # d.W(0x35EC), d.SetW(0x366C + i * 4, ...), device.F32(0x3218)
    re.compile(r'\b((?:d|dev|device|dd|src|dst)\.(?:Set)?(?:W|B|H|F|Q|D|U64|S|F32)\(\s*)' + NUM + r'\b'),
    # d.va + 0x3050, d.bytes + 0x3440, device + 0x3B6C, device_va + ...
    re.compile(r'\b((?:d\.va|d\.bytes|device|device_va|bytes)\s*\+\s*)' + NUM + r'\b'),
    # WriteU32(bytes, 21768, ...)
    re.compile(r'\b(WriteU32\(\s*bytes\s*,\s*)' + NUM + r'\b'),
    # for (u32 offset = 0x35A0; offset <= 0x35B4; offset += 4)
    re.compile(r'\b(offset\s*(?:=|<=|<)\s*)' + NUM + r'\b'),
]
# Constants that are device offsets: kDeviceOffset* / kDeviceSnapshotSize anywhere, the off:: namespace of
# d3d_native.h, and these (d3d_native_state.cpp's state storage, device.cpp's own).
OFFSET_NAME = re.compile(r'^(?:kDeviceOffset\w*|kDeviceSnapshotSize)$')
OFFSET_NAMES = {
    'kBlendState', 'kBlendFlags', 'kScissorTestEnable', 'kColorWriteEnable', 'kZEnable', 'kStencilEnable',
    'kPointSpriteEnable', 'kPointSize', 'kPointSizeMin', 'kPointSizeMax', 'kHighPrecisionBlend', 'kPresentInterval',
    'kBehaviorFlags2', 'kMaxAnisotropy', 'kMaxMipLevel', 'kMinMipLevel', 'kZFilter', 'kViewport', 'kVportXScale',
    'kVportZScale', 'kStreamDeclStrides', 'kVertexDeclaration', 'kVertexShader', 'kPixelShader', 'kGprAllocation',
    'kScreenExtentQueryMode', 'kExportAddress', 'kExportCount', 'kExportNanVertexDone', 'kExportFlags',
}
# Literals inside the moved range that are not device offsets (masks, tunables, tables): (file name, line regex).
NOT_OFFSETS = [
    ('display_sync.cpp', r'\.range\(0, 16000\)'),
    ('banjo_renut_tuning.cpp', r'\.range\(0, 16384\)'),
    ('banjo_renut_tuning.cpp', r'since_build >= 15000'),
    ('d3d_native_state.cpp', r'Dirty\(kPendingMisc, 0x3C00\)'),
    ('resource.cpp', r'0x2000, 0x4000, 0x8000\}'),
    ('*', r'&\s*0x3FFF\b'),
]


def split_comment(line):
    """(code, comment) at the first // outside a string."""
    in_str = False
    for i, ch in enumerate(line):
        if ch == '"' and (i == 0 or line[i - 1] != '\\'):
            in_str = not in_str
        elif not in_str and line.startswith('//', i):
            return line[:i], line[i:]
    return line, ''


def in_range_literals(code):
    out = []
    for m in re.finditer(r'(?<![\w.])(0x[0-9A-Fa-f]+|\d+)(?:u|U|ull|ULL)?(?![\w.])', code):
        v = value_of(m.group(1))
        if INSERT <= v <= END:
            out.append(m.group(1))
    return out


def not_offset(path, line):
    base = os.path.basename(path)
    return any((f == '*' or f == base) and re.search(rx, line) for f, rx in NOT_OFFSETS)


def process(path, report):
    """The file's translated text and its changes [(line number, old, new)]."""
    text = open(path, encoding='utf-8', newline='').read()
    rel = os.path.relpath(path, ROOT).replace('\\', '/')
    if MARK in text:
        report['done'].append(rel)
        return text, []
    eol = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(eol)
    changes = []
    in_off = False
    for i, line in enumerate(lines):
        if re.match(r'^namespace off \{', line):
            in_off = True
        elif in_off and re.match(r'^\}\s*//\s*namespace off', line):
            in_off = False
        code, comment = split_comment(line)
        new = code
        handled = False
        m = CONST.match(code)
        if m:
            name, val = m.group(2), m.group(3)
            if name in GLOBALS:
                old, want = GLOBALS[name]
                if value_of(val) == value_of(old):
                    new = m.group(1) + want + m.group(4)
                elif value_of(val) != value_of(want):
                    report['unclassified'].append('%s:%d: %s = %s, expected Futari\'s %s' % (rel, i + 1, name, val, old))
                handled = True
            elif name in KEEP:
                report['kept'].append('%s:%d: %s (%s)' % (rel, i + 1, name, KEEP[name]))
                handled = True
            elif OFFSET_NAME.match(name) or name in OFFSET_NAMES or in_off:
                new = m.group(1) + translate(val) + m.group(4)
                handled = True
        if not handled:
            m = CONST_ARRAY.match(code)
            if m:
                new = m.group(1) + ', '.join(translate(v.strip()) for v in m.group(3).split(',')) + m.group(4)
                handled = True
        if not handled:
            for rx in CONTEXTS:
                new = rx.sub(lambda a: a.group(1) + translate(a.group(2)), new)
            for old, want, what in IMPORTS:
                new = new.replace(old, want)
        # Literals in the moved range that no rule translated (a rule leaves the same number of literals).
        olds, news = in_range_literals(code), in_range_literals(new) if new != code else None
        for k, lit in enumerate(olds):
            if value_of(lit) == END:
                report['unclassified'].append('%s:%d: 0x552C, Futari\'s padding (no field): %s' % (rel, i + 1, line.strip()))
            elif (news is None or k >= len(news) or news[k] == lit) and not not_offset(path, code):
                report['unclassified'].append('%s:%d: %s in: %s' % (rel, i + 1, lit, line.strip()))
        for lit in in_range_literals(comment):
            report['comments'].append('%s:%d: %s' % (rel, i + 1, comment.strip()[:140]))
            break
        if new != code:
            lines[i] = new + comment
            changes.append((i + 1, line, lines[i]))
    out = eol.join(lines)
    if changes:
        if FUTARI_MARK in out:
            out = out.replace(FUTARI_MARK, FUTARI_MARK + eol + MARK, 1)
        else:
            k = out.find('*/')
            out = out[:k + 2] + eol + MARK + out[k + 2:] if k >= 0 else MARK + eol + out
    return out, changes


def sources():
    paths = []
    for ext in ('*.cpp', '*.h'):
        paths += glob.glob(os.path.join(ENGINE, '**', ext), recursive=True)
    return sorted(p for p in paths if os.path.basename(p) not in SKIP_FILES)


D3D_RANGE = (0x82253000, 0x82278000)  # Guwange's Direct3D in default.xex


def guwange_accesses():
    """{immediate: count} of displacements and addi immediates in Guwange's Direct3D (app/generated/default)."""
    counts = {}
    func = None
    for path in glob.glob(os.path.join(GEN, '*_recomp.*.cpp')):
        for line in open(path, encoding='utf-8', errors='replace'):
            m = re.match(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)', line)
            if m:
                func = int(m.group(1), 16)
                continue
            if func is None or not D3D_RANGE[0] <= func < D3D_RANGE[1] or not line.startswith('\t// '):
                continue
            m = re.search(r',(-?\d+)\(r(\d+)\)$', line.rstrip()) or re.match(r'^\t// addi r\d+,r(\d+),(-?\d+)$', line)
            if not m:
                continue
            if line.startswith('\t// addi'):
                base, imm = m.group(1), int(m.group(2))
            else:
                imm, base = int(m.group(1)), m.group(2)
            if base != '1':
                counts[imm] = counts.get(imm, 0) + 1
    return counts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--write', action='store_true', help='apply the changes (default: a dry run)')
    ap.add_argument('--pins', action='store_true', help='list Guwange\'s accesses at every moved offset')
    a = ap.parse_args()
    report = {'unclassified': [], 'comments': [], 'kept': [], 'done': []}
    total = 0
    moved = set()
    for path in sources():
        out, changes = process(path, report)
        rel = os.path.relpath(path, ROOT).replace('\\', '/')
        for n, old, new in changes:
            print('%s:%d: %s\n%s   -> %s' % (rel, n, old.strip(), ' ' * len('%s:%d:' % (rel, n)), new.strip()))
            for o, w in zip(re.findall(r'(?<![\w.])(0x[0-9A-Fa-f]+|\d{4,5})(?![\w.])', old),
                            re.findall(r'(?<![\w.])(0x[0-9A-Fa-f]+|\d{4,5})(?![\w.])', new)):
                if o != w and INSERT <= value_of(o) < END:
                    moved.add((value_of(o), value_of(w)))
        total += len(changes)
        if changes and a.write:
            open(path, 'w', encoding='utf-8', newline='').write(out)
    print('\n%d lines %s' % (total, 'changed' if a.write else 'would change (dry run; --write applies)'))
    for d in report['done']:
        print('already translated (marker): %s' % d)
    for k in report['kept']:
        print('kept: %s' % k)
    for c in report['comments']:
        print('comment, not changed: %s' % c)
    for u in report['unclassified']:
        print('UNCLASSIFIED (check by hand): %s' % u)
    if a.pins:
        counts = guwange_accesses()
        print('\nGuwange accesses (Direct3D 0x%08X-0x%08X) at each moved offset:' % D3D_RANGE)
        for old, new in sorted(moved):
            n = counts.get(new, 0)
            print('  Futari %5d (0x%04X) -> %5d (0x%04X): %s' % (old, old, new, new,
                                                                 '%d accesses' % n if n else 'UNPINNED (array element?)'))
    return 1 if report['unclassified'] else 0


if __name__ == '__main__':
    sys.exit(main())
