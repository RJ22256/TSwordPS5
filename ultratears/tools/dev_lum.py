"""Development: mean linear colour of HDR targets in a TOTK_DUMP folder (the dump stores float formats Reinhard-mapped).

usage: python tools/dev_lum.py <dump dir> [name filter]
"""
import glob
import os
import sys

from PIL import Image


def linear(t):
    t = t / 255.0
    return t / (1 - t) if t < 0.999 else 99.0


def main():
    folder = sys.argv[1]
    filt = sys.argv[2] if len(sys.argv) > 2 else ""
    for f in sorted(glob.glob(os.path.join(folder, "*.bmp"))):
        name = os.path.basename(f)
        if filt not in name:
            continue
        fmt = name.split("_")[1]
        im = Image.open(f).convert("RGB").resize((64, 36))
        px = list(im.get_flattened_data()) if hasattr(im, "get_flattened_data") else list(im.getdata())
        conv = linear if fmt in ("3f", "29") else (lambda v: v / 255.0)
        m = [sum(conv(p[i]) for p in px) / len(px) for i in range(3)]
        print(f"{name:28s} mean {m[0]:7.3f} {m[1]:7.3f} {m[2]:7.3f}")


if __name__ == "__main__":
    main()
