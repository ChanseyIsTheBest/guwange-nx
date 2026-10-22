#!/usr/bin/env python3
"""Guwange's post-codegen steps, run on every generated folder after every code generation (tools/codegen.ps1 and the
build's own codegen command, app/CMakeLists.txt). Each step is idempotent.

Correctness (always):
  1. tools/spin_forever.py   guest loops that can never end made explicit (clang deleted main's spin: the title quit
                             at its first boot)
Performance (the user, 2026-10-22: "do all the codegen optimisations"; unless --correctness-only):
  2. tools/post_codegen.py   futari-nx's steps 0-16 (sa2-nx's CPU rounds and banjo-nx's codegen round) on Guwange's
                             five modules, without step 12 (Futari's localized copies). Each step has its switch back
                             in app/CMakeLists.txt (GUWANGE_GEN_* / GUWANGE_SWITCH_*); GUWANGE_PERF_CODEGEN_POST=OFF
                             runs step 1 only, the A side of the A/B pair.

  python tools/guwange_post_codegen.py [--check] [--correctness-only] [--no-choice]
--check: step 1 reports what it would change, nothing else runs.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import project  # noqa: E402


def main():
    gen = os.path.join(ROOT, 'app', 'generated')
    folders = [os.path.join(gen, t) for t in project.trees() if os.path.isdir(os.path.join(gen, t))]
    check = '--check' in sys.argv
    r = subprocess.run([sys.executable, '-P', os.path.join(ROOT, 'tools', 'spin_forever.py'), *folders,
                        *(['--check'] if check else [])])
    if r.returncode != 0 or check or '--correctness-only' in sys.argv:
        return r.returncode
    extra = [a for a in sys.argv[1:] if a == '--no-choice']
    r = subprocess.run([sys.executable, '-P', os.path.join(ROOT, 'tools', 'post_codegen.py'), *extra], cwd=ROOT)
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
