#!/usr/bin/env python3
"""Writes Guwange's Switch configs from the enhancement table (app/src/guwange_enhancements.cpp, kTable), so a new
enhancement only has to be added in one place (guwange-nx, 2026-10-21; futari-nx's tools/make_enhancement_configs.py,
lean):

  app/guwange_switch_defaults.toml  the release's FIXED settings, compiled into the NRO (app/CMakeLists.txt,
                                    src/guwange_app.h BuiltInConfig) and applied before guwange.toml: every enhancement
                                    on, but the kExplicitOnly ones off (Entry's comment: written OFF by the generated
                                    configs; Futari's audio declick, 2026-10-22), logging and profiling off
  app/guwange_switch.toml           (hand-written, not generated) the player's guwange.toml: the input options only
  app/configs/guwange_test.toml     the test config: the player's options + the log (logs/ on the SD card)
  app/configs/guwange_cpu_off.toml  the test config with every CPU enhancement off (the A/B baseline)
  app/configs/guwange_all_off.toml  the test config with both masters off

  python tools/make_guwange_configs.py
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse_table(path):
    text = open(path, encoding='utf-8').read()
    rows = re.findall(r'\{"([a-z0-9_]+)",\s*(kCpu|kGpu),\s*"((?:[^"\\]|\\.)*)",\s*"([^"]*)",\s*"([^"]*)"'
                      r'(,\s*kExplicitOnly)?\}', text)
    if not rows:
        raise SystemExit('no rows found in kTable of ' + path)
    return [dict(key=k, cpu=g == 'kCpu', on=on, off=off, explicit=bool(x)) for k, g, w, on, off, x in rows]


def value(v):
    return v if v in ('true', 'false') or re.fullmatch(r'-?\d+', v) else '"%s"' % v


BUILTIN_HEADER = """# Guwange for the Switch: the fixed settings, compiled into the NRO (app/CMakeLists.txt, src/guwange_app.h
# BuiltInConfig) and applied before the SD card's guwange.toml, which holds the player's options (app/guwange_switch.toml)
# and may still override any key here for testing (app/configs).
# Written by tools/make_guwange_configs.py from app/src/guwange_enhancements.cpp: edit the table there, not this file.
# Logging and profiling are off: no logs/ folder on the SD card.

# Game. The renderer: Renut's Direct3D-level renderer on deko3d (no PM4 ring, no EDRAM model). The NRO links no Vulkan
# driver: deko3d only.
futari_renderer = "renut"
renut_gpu_backend = "deko3d"
gpu_allow_invalid_fetch_constants = true
license_mask = 1

# No logging, no profiling
log_level = "off"
log_to_file = false
switch_profile_stacks = false
switch_profile_report = false
renut_memory_report = 0
audio_pop_detect = false

# Enhancement masters (false turns every enhancement of that group off)
guwange_cpu_enhancements = true
guwange_noncpu_enhancements = true
"""

TEST_HEADER = """# Guwange (Switch) - {title}
# Written by tools/make_guwange_configs.py: copy it to sdmc:/switch/guwange-nx/ as guwange.toml (the game reads only
# that name). The startup log line "[enhancements]" says which enhancements were active.

log_level = "info"
log_to_file = true  # the release writes no log (its built-in settings)
renut_gpu_profile = 10  # Renut renderer: GPU time per frame by kind of work, every 10 s
{extra}
"""


def main():
    rows = parse_table(os.path.join(ROOT, 'app', 'src', 'guwange_enhancements.cpp'))
    app = os.path.join(ROOT, 'app')
    lines = [BUILTIN_HEADER]
    for cpu, title in ((True, 'CPU enhancements'), (False, 'Non-CPU enhancements')):
        lines.append(f'\n# {title}\n')
        for r in rows:
            if r['cpu'] == cpu:
                lines.append(f"{r['key']} = {value(r['off'] if r['explicit'] else r['on'])}\n")
    with open(os.path.join(app, 'guwange_switch_defaults.toml'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(''.join(lines))
    player = open(os.path.join(app, 'guwange_switch.toml'), encoding='utf-8').read()
    tests = {
        'guwange_test.toml': ('TEST: the release settings with the log on', ''),
        'guwange_cpu_off.toml': ('TEST: every CPU enhancement off (A/B baseline), log on',
                                 'guwange_cpu_enhancements = false\n'),
        'guwange_all_off.toml': ('TEST: every enhancement off (both masters), log on',
                                 'guwange_cpu_enhancements = false\nguwange_noncpu_enhancements = false\n'),
        # The console CPU profile: tools/switch_hot_functions.py logs/rex/rex_profile.log --map out/switch/guwange.map
        'guwange_profile.toml': ('PROFILE: the release settings, log and CPU sampler on',
                                 'switch_profile_report = true  # logs/rex/rex_profile.log\n'
                                 'switch_profile_stacks = true  # pc and stack of every busy thread at 1 kHz\n'),
    }
    os.makedirs(os.path.join(app, 'configs'), exist_ok=True)
    for name, (title, extra) in tests.items():
        # The test keys go before the player's file: its [input_p1] / [input_p2] tables must come last in TOML.
        with open(os.path.join(app, 'configs', name), 'w', encoding='utf-8', newline='\n') as f:
            f.write(TEST_HEADER.format(title=title, extra=extra) + '\n' + player)
    n_cpu = sum(r['cpu'] for r in rows)
    print(f'{len(rows)} enhancements ({n_cpu} CPU, {len(rows) - n_cpu} non-CPU); wrote guwange_switch_defaults.toml '
          f'and configs/{", configs/".join(tests)}')


if __name__ == '__main__':
    main()
