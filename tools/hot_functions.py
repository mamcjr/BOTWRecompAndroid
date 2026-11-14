#!/usr/bin/env python3
"""The game functions play sessions entered, for the hot/cold game code build (runtime/gamebuild.c, Android;
LASRecompAndroid's, per game version here).

Collect on the phone (counts from process start, the file rewritten every 1800 frames while playing), e.g. for 1.8.2:
  bash android/run_ver.sh v182 3600 \\
    'LARECOMP_FNPROF=0:999999999,LARECOMP_FNPROF_EVERY=1800,LARECOMP_FNPROF_OUT=/sdcard/Android/data/com.zbotw.recomp/files/v182/build/fnprof.txt'
play through as many places as possible, then
  adb pull /sdcard/Android/data/com.zbotw.recomp/files/v182/build/fnprof.txt session1.txt
Merge one or more sessions of one game version into the list the APK ships (android/botwrecompandroid/app/
build.gradle.kts copies tools/hot_functions_*.txt; runtime/gamebuild.c picks the one of the game it builds):
  hot_functions.py session1.txt [session2.txt ...] [-o tools/hot_functions_182.txt]

The sessions name their game version ('# game_version', runtime/fnprof.c): versions differ in what lives where, so
they can't be mixed, and the list is written to tools/hot_functions_<version>.txt unless -o says otherwise.
Inputs may also be lists of "<module> <hex offset>" lines (main / sdk; no counts).
Addresses are written as the generated code names its functions (f_<address>: main at 0x7000000000, sdk at
0x7004000000), whatever the guest window was in the session (its '# main_base' line). Only addresses: no game code."""
import argparse
import sys
from pathlib import Path

NAMED_MAIN = 0x7000000000

p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('sessions', nargs='+', type=Path, help='fnprof.txt files (runtime/fnprof.c)')
p.add_argument('-o', '--out', type=Path, help='the list (default: tools/hot_functions_<version>.txt)')
p.add_argument('--version', type=int, help="the game version of inputs that don't name one (100, 160, 182)")
p.add_argument('--min-count', type=int, default=1, help='entries a function needs, summed over the sessions')
args = p.parse_args()

MODULES = {'main': NAMED_MAIN, 'sdk': NAMED_MAIN + 0x4000000}

counts = {}
versions = set()
for path in args.sessions:
    base = NAMED_MAIN  # (no '# main_base' line: a desktop session, addresses already as named)
    version = args.version
    for line in path.read_text().splitlines():
        if line.startswith('# main_base '):
            base = int(line.split()[2], 16)
            continue
        if line.startswith('# game_version '):
            version = int(line.split()[2]) or args.version
            continue
        if not line.strip() or line.startswith('#'):
            continue
        f = line.split()
        if f[0] in MODULES:  # ("<module> <offset>", no counts: each one entered)
            named, count = MODULES[f[0]] + int(f[1], 16), args.min_count
        else:
            named, count = int(f[0], 16) - base + NAMED_MAIN, int(f[1])
        if not NAMED_MAIN <= named < NAMED_MAIN + 0x8000000:  # (main and sdk: anything else isn't generated code)
            continue
        counts[named] = counts.get(named, 0) + int(count)
    versions.add(version)

if len(versions) != 1 or None in versions:
    sys.exit(f'hot_functions.py: the sessions are of game versions {sorted(versions, key=str)} - one version per list '
             '(--version for inputs that name none)')
version = versions.pop()
out = args.out or Path(__file__).resolve().parent / f'hot_functions_{version}.txt'
hot = sorted(a for a, n in counts.items() if n >= args.min_count)
out.write_text(f'# {len(hot)} hot game functions of game version {version} from {len(args.sessions)} session(s): '
               'tools/hot_functions.py\n' + ''.join(f'{a:x}\n' for a in hot))
print(f'{len(hot)} hot functions (game version {version}) -> {out}')
