#!/usr/bin/env python3
"""futari-nx (2026-10-20, peggle2-nx's tools/renut_release_shaders.py): the Switch build's Renut shader files without
translated game shaders, for a build that can be distributed (out/renut_switch_release/), and the file the web
installer makes in their place, for tests.

    python tools/renut_release_shaders.py [--glsl out/deko3d_shaders/glsl_opt] [--cache out/renut_switch/shader_cache_switch.cpp]

Writes into out/renut_switch_release/:
  deko3d_glsl.bin, deko3d_glsl_switch.cpp   the GLSL package built into the NRO: only BUILT_IN below (peggle2-nx's
                            design: its Bink movie shaders)
  shader_cache_switch.cpp   the shader table (numbers: hashes, fetch masks, vertex input formats) without the SPIR-V
                            (the deko3d back end never reads it; the PC's Vulkan one does)
  sd/deko3d_glsl.bin        the package of the other game shaders, which the web installer makes from the player's
                            files and puts beside the NRO (this one is the Python tools' copy: game data, for tests
                            only, never distributed)
guwange-nx (2026-10-22): of Guwange's 87 shaders the installer makes 78 from the player's files (guwange-nx-installer):
the 68 containers default.xex stores and the 10 of the sprite effects (game_res/shader/*.cfx and pkgres/sprite_i_360.cfx
in res/DSD000.bin and DSD100.bin, decompressed with the game's LZSS, sub_821FDE00). The other 9 are in no game file
(not in the executables' containers, not in any archive entry: the runtime builds them, one's microcode is near the
start of default.xex's .rdata like Futari's D3D raw programs), so the NRO carries them. app/cmake/guwange_renut_gpu.cmake
(GUWANGE_RENUT_RELEASE_SHADERS) builds with these files.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The shaders no file of the player's holds (see above): built into the NRO.
BUILT_IN = ['ps_032176CF3775813D', 'ps_9762A7E17B1085DA', 'ps_D3A0C9040131A779', 'vs_0181A67E4F71CB15',
            'vs_06ADC7974A7ED08A', 'vs_4B324398D9B2D460', 'vs_7B1CA326B071AA2A', 'vs_CA0583A0111C853D',
            'vs_D901AB8F9EDBD45D']


def package(glsl_dir, names, out_dir):
    """tools/deko3d_shader_package.py over the given shaders only."""
    os.makedirs(out_dir, exist_ok=True)
    src = os.path.join(out_dir, 'glsl')
    shutil.rmtree(src, ignore_errors=True)
    os.makedirs(src)
    for n in names:
        shutil.copyfile(os.path.join(glsl_dir, n), os.path.join(src, n))
    subprocess.run([sys.executable, '-P', os.path.join(ROOT, 'tools', 'deko3d_shader_package.py'), '--glsl', src,
                    '--out', out_dir], check=True)
    shutil.rmtree(src)


def strip_spirv(text):
    """shader_cache_switch.cpp without the SPIR-V: the assembler's .incbin goes, its sizes are 0, and every entry's
    SPIR-V offset and size (the 4th and 5th numbers of g_shaderCacheEntries' lines) are 0."""
    text, n = re.subn(r'// banjo-nx: the compressed SPIR-V.*?\.previous\\n"\);\n',
                      'const uint8_t g_compressedSpirvCache[] = {0};  // futari-nx: no SPIR-V (deko3d reads none)\n',
                      text, flags=re.S)
    if n != 1:
        sys.exit('renut_release_shaders: the SPIR-V block of shader_cache_switch.cpp not found')
    text = re.sub(r'(g_spirvCache(?:Compressed|Decompressed)Size\s*=\s*)\d+', r'\g<1>0', text)

    def entry(m):
        fields = m.group(2).split(', ')
        fields[3] = fields[4] = '0'
        return m.group(1) + ', '.join(fields)
    text, n = re.subn(r'^(\t\{ )(0x[0-9A-F]+, \d+, \d+, \d+, \d+)', entry, text, flags=re.M)
    if not n:
        sys.exit('renut_release_shaders: no entry of g_shaderCacheEntries found')
    if 'incbin' in text:
        sys.exit('renut_release_shaders: an .incbin is left in the shader table')
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--glsl', default=os.path.join(ROOT, 'out', 'deko3d_shaders', 'glsl_opt'))
    ap.add_argument('--cache', default=os.path.join(ROOT, 'out', 'renut_switch', 'shader_cache_switch.cpp'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'renut_switch_release'))
    a = ap.parse_args()
    names = sorted(f for f in os.listdir(a.glsl) if re.fullmatch(r'(vs|ps)_[0-9A-F]{16}\.glsl', f))
    if not names:
        sys.exit(f'renut_release_shaders: no shaders in {a.glsl}')
    built_in = [n + '.glsl' for n in BUILT_IN]
    missing = [n for n in built_in if n not in names]
    if missing:
        sys.exit(f'renut_release_shaders: {", ".join(missing)} not in {a.glsl}')
    os.makedirs(a.out, exist_ok=True)
    package(a.glsl, built_in, a.out)
    package(a.glsl, [n for n in names if n not in built_in], os.path.join(a.out, 'sd'))
    os.remove(os.path.join(a.out, 'sd', 'deko3d_glsl_switch.cpp'))  # the installer's file is never built in
    text = open(a.cache, encoding='utf-8').read()
    with open(os.path.join(a.out, 'shader_cache_switch.cpp'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(strip_spirv(text))
    print(f'renut_release_shaders: {len(built_in)} shaders built in, beside the NRO {len(names) - len(built_in)} game '
          f'shaders -> {a.out}')


if __name__ == '__main__':
    main()
