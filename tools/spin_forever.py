#!/usr/bin/env python3
"""Guest loops that can never end, made explicit (post-codegen step; guwange-nx, 2026-10-21).

A loop of the generated code whose header is followed only by compares and a branch back to it (the guest's
`cmpwi cr6,r11,0; beq cr6,.` after loading r11 once, or a plain `b .`) changes nothing it tests: entered once, it never
ends. On the Xbox 360 that is a real spin (Guwange's main, 0x821E9814, keeps the title alive this way while the game
runs on its other threads). In C++ it is a loop without side effects, which the compiler may assume to terminate:
clang deleted it, main returned 0 and the title quit at its first boot. The barrier step (tools/loop_barriers.py)
leaves such a loop without a barrier on purpose (it reads no memory), so nothing else covers it.

The backward goto becomes REX_SPIN_FOREVER(): a call the compiler cannot remove that never returns (the app defines
rex_spin_forever(): the thread sleeps for good instead of burning a core). Only loops whose every line between the
header and the goto is a comment, a CR compare (cr*.compare...) or the goto itself are changed: nothing else can be
assumed invariant. The pch gets the macro.

  python tools/spin_forever.py app/generated/default [app/generated/<core> ...] [--check]
"""
import glob
import os
import re
import sys

MACRO = ('// guwange-nx (tools/spin_forever.py): a guest loop that can never end, kept by a call the compiler cannot '
         'remove (the thread sleeps for good).\n'
         '[[noreturn]] void rex_spin_forever();\n'
         '#define REX_SPIN_FOREVER() rex_spin_forever()\n')
COMPARE = re.compile(r'^\tcr\d\.compare<[^>]+>\(.*\);$')
GOTO = re.compile(r'^\t(?:if \((?:!?cr\d\.(?:eq|lt|gt|so))\) )?goto (loc_[0-9A-F]+);$')


def process(path, check):
    lines = open(path, encoding='utf-8', errors='replace', newline='').read().split('\n')
    changed = []
    i = 0
    while i < len(lines):
        m = re.match(r'^(loc_[0-9A-F]+):$', lines[i])
        if not m:
            i += 1
            continue
        label = m.group(1)
        j = i + 1
        ok = True
        while j < len(lines):
            line = lines[j]
            g = GOTO.match(line)
            if g:
                if g.group(1) != label:
                    ok = False
                break
            if line.startswith('\t//') or COMPARE.match(line) or not line.strip():
                j += 1
                continue
            ok = False
            break
        if ok and j < len(lines) and GOTO.match(lines[j]):
            new = re.sub(r'goto ' + label + ';', 'REX_SPIN_FOREVER();', lines[j])
            changed.append((label, lines[j].strip(), new.strip()))
            lines[j] = new
            i = j + 1
            continue
        i += 1
    if changed and not check:
        open(path, 'w', encoding='utf-8', newline='').write('\n'.join(lines))
    return changed


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    check = '--check' in sys.argv
    total = 0
    for folder in args:
        found = []
        for path in sorted(glob.glob(os.path.join(folder, '*_recomp.*.cpp'))):
            for label, old, new in process(path, check):
                found.append(f'{os.path.basename(path)} {label}: {old} -> {new}')
        pchs = glob.glob(os.path.join(folder, '*_pch.h'))
        if found and pchs and not check:
            text = open(pchs[0], encoding='utf-8', newline='').read()
            if 'REX_SPIN_FOREVER' not in text:
                open(pchs[0], 'a', encoding='utf-8', newline='').write('\n' + MACRO)
        for f in found:
            print(f'{folder}: {f}')
        total += len(found)
    print(f'{total} never-ending loops {"found" if check else "made explicit"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
