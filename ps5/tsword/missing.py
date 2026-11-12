#!/usr/bin/env python3
"""ps5/tsword/missing.py --sdk <payload SDK> <archive>... - what the TSword runtime (TotK Recomp) (and its dependencies' archives)
still needs from outside them: the symbols they use that none of them defines, less what the console's libraries
and the SDK's libc export. Grouped by what will provide them (a Markdown report, for the CI summary):

  SDL3      the title's SDL subset (WSword PS5's sdl_ps5.c)
  Vulkan    RADV, linked into the title (PS5_Vulkan)
  linker    section bounds, --wrap's __real_ names, RADV's vk_icdGetInstanceProcAddr: the final link's
  other     anything else: what the console lacks and needs a replacement
"""
import argparse
import glob
import os
import subprocess
import sys


def nm(tool, path, dynamic=False):
    args = [tool, "-A", "-D" if dynamic else "--no-sort", path]
    if dynamic:
        args.append("--defined-only")
    out = subprocess.run(args, capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            yield parts[-2], parts[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True)
    ap.add_argument("archives", nargs="+")
    a = ap.parse_args()
    tool = os.path.join(a.sdk, "bin", "llvm-nm")
    used, defined = set(), set()
    for ar in a.archives:
        for kind, name in nm(tool, ar):
            if kind == "U":
                used.add(name)
            elif kind in "TDBRWVuiCSdbrt" and len(kind) == 1:
                defined.add(name)
    system = set()
    lib = os.path.join(a.sdk, "target", "lib")
    for so in glob.glob(os.path.join(lib, "*.so")):
        system.update(n for _, n in nm(tool, so, dynamic=True))
    for st in glob.glob(os.path.join(lib, "*.a")):
        system.update(n for k, n in nm(tool, st) if k in ("T", "W", "D", "B", "R", "V"))
    missing = sorted(used - defined - system)
    groups = {"SDL3": [], "Vulkan": [], "linker": [], "other": []}
    for s in missing:
        if s.startswith("SDL_"):
            groups["SDL3"].append(s)
        elif s.startswith(("__start_", "__stop_", "__real_")) or s in ("__dso_handle", "vk_icdGetInstanceProcAddr"):
            groups["linker"].append(s)
        elif s.startswith("vk"):
            groups["Vulkan"].append(s)
        else:
            groups["other"].append(s)
    print(f"### TSword runtime (TotK Recomp) for the PS5: {len(missing)} symbols from outside\n")
    for g, names in groups.items():
        print(f"- **{g}**: {len(names)}" + (f" — {' '.join(names[:400])}" if names and g != "Vulkan" else ""))
    return 0


sys.exit(main())
