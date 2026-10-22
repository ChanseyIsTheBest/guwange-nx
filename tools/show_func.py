#!/usr/bin/env python3
"""Prints a recompiled function's PPC listing (the instruction comments of the generated C++), with addresses.

    python tools/show_func.py <address> [--gen app/generated/default] [--calls]

--calls prints only its bl / b / bctr lines (the call graph one level down). guwange-nx, 2026-10-21.
"""
import argparse
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def listing(gen, addr):
    key = f'DEFINE_REX_FUNC(sub_{addr:08X})'
    for path in glob.glob(os.path.join(gen, '*_recomp.*.cpp')):
        text = open(path, errors='replace').read()
        i = text.find(key)
        if i < 0:
            continue
        body = text[i:text.index('\n}\n', i)]
        out, cur = [], addr
        for line in body.split('\n')[1:]:
            m = re.match(r'loc_([0-9A-F]+):', line)
            if m:
                cur = int(m.group(1), 16)
                continue
            m = re.match(r'\t// (\S.*)$', line)
            if m and not m.group(1).startswith(('PPC_', '0x', 'ERROR')):
                out.append((cur, m.group(1).strip()))
                cur += 4
        return out
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('address', type=lambda v: int(v, 16))
    ap.add_argument('--gen', default=os.path.join(ROOT, 'app', 'generated', 'default'))
    ap.add_argument('--calls', action='store_true')
    a = ap.parse_args()
    ins = listing(a.gen, a.address)
    if ins is None:
        raise SystemExit(f'sub_{a.address:08X} is not a function start in {a.gen}')
    for addr, text in ins:
        if not a.calls or re.match(r'(bl|b|bctr|bctrl|bcctrl)\b', text):
            print(f'{addr:08X}  {text}')


if __name__ == '__main__':
    main()
