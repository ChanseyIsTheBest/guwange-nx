#!/usr/bin/env python3
"""The port's own names for the post-codegen tools (guwange-nx, 2026-10-22), from the one *_manifest.toml in app/:
the project name (the prefix of generated/<module>/<name>_pch.h, <name>_recomp.N.cpp), the generated module folders,
each module's XEX/DLL file and the code generator's image dump of it (REXGLUE_DUMP_IMAGE: out/default_image.bin for
the entry point, out/<the module file's stem>_image.bin for a module, as tools/codegen.ps1 writes them). futari-nx's
tools named these for Futari; this file lets the same tools run on another port's tree.

  python tools/project.py      (prints what it finds)
"""
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, 'app')


def manifest_path():
    """app/<project>_manifest.toml for the project app/CMakeLists.txt builds (a tree cloned from another port may
    still hold that port's manifest)."""
    found = sorted(glob.glob(os.path.join(APP, '*_manifest.toml')))
    cmake = os.path.join(APP, 'CMakeLists.txt')
    if os.path.isfile(cmake):
        m = re.search(r'^\s*project\(\s*(\w+)', open(cmake, encoding='utf-8').read(), re.M)
        if m and os.path.join(APP, m.group(1) + '_manifest.toml') in found:
            return os.path.join(APP, m.group(1) + '_manifest.toml')
    if len(found) != 1:
        raise SystemExit('project.py: expected one app/*_manifest.toml, found %s' % (found or 'none'))
    return found[0]


def _manifest():
    return open(manifest_path(), encoding='utf-8').read()


def name():
    m = re.search(r'^\[project\]\s*\n(?:[^\[]*?\n)?name\s*=\s*"([^"]+)"', _manifest(), re.M)
    if not m:
        raise SystemExit('project.py: no [project] name in ' + manifest_path())
    return m.group(1)


def modules():
    """[(tree, file path, is the entry point)] in the manifest's order: the generated folder's name
    (out_directory_path = "generated/<tree>") and the binary it is made from."""
    out = []
    for block in re.split(r'^(?=\[)', _manifest(), flags=re.M):
        entry = block.startswith('[entrypoint]')
        if not (entry or block.startswith('[[modules]]')):
            continue
        f = re.search(r'^file_path\s*=\s*"([^"]+)"', block, re.M)
        o = re.search(r'^out_directory_path\s*=\s*"generated/(\w+)"', block, re.M)
        if f and o:
            out.append((o.group(1), os.path.normpath(os.path.join(APP, f.group(1))), entry))
    return out


def trees():
    return tuple(t for t, _, _ in modules())


def xex_files():
    return {t: path for t, path, _ in modules()}


def image_path(tree):
    for t, path, entry in modules():
        if t == tree:
            stem = 'default' if entry else os.path.splitext(os.path.basename(path))[0]
            return os.path.join(ROOT, 'out', '%s_image.bin' % stem)
    return os.path.join(ROOT, 'out', '%s_image.bin' % tree)


def unbuilt(path):
    """Whether `path` is a source another port's tree left here, not built for this project: futari-nx's src/me,
    src/native and app/src/futari_* files, another project's tomls in app/. The tools that collect guest addresses the
    sources name (hooks, natives) skip them: they name the other game's functions."""
    if name() == 'futari':
        return False
    path = os.path.normcase(os.path.abspath(path))
    src = os.path.normcase(os.path.join(APP, 'src'))
    for folder in ('me', 'native'):
        if path.startswith(os.path.join(src, folder) + os.sep):
            return True
    base = os.path.basename(path)
    if os.path.dirname(path) == src and base.startswith('futari_'):
        return True
    if base in ('futari_d3d_functions.txt', 'futari_renut_core_hooks.cpp'):  # Futari's map, Futari's core forwarding
        return True
    return os.path.dirname(path) == os.path.normcase(APP) and base.endswith('.toml') and \
        not base.startswith(name() + '_')


if __name__ == '__main__':
    print('project %s (%s)' % (name(), manifest_path()))
    for t, path, entry in modules():
        print('  %-16s %s%s -> %s' % (t, path, ' (entry point)' if entry else '', image_path(t)))
