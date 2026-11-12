"""(TSword PS5) Where the three code modules are lifted to: src/runtime/rt.h MAIN_BASE, SUBSDK0_BASE, SDK_BASE. The PC's
layout puts main at 0x8_0000_0000; the PS5's (its system modules sit there) at 0x18_0000_0000, which the PS5 export
asks for with ULTRS_MAIN_BASE (ps5/ut-overlay/tools/build_ps5.py). Addresses written for the PC's layout (the patch
table, the lifter's exceptions) go through pc()."""
import os

PC_MAIN = 0x800000000
MAIN = int(os.environ.get('ULTRS_MAIN_BASE', hex(PC_MAIN)), 0)
BASES = {'main': MAIN, 'subsdk0': MAIN + 0x4000000, 'sdk': MAIN + 0x5000000}


def pc(a):
    """An address of the PC's layout, in this one."""
    return a - PC_MAIN + MAIN
