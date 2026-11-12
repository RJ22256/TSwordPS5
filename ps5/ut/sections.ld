/* ps5/ut/sections.ld - UltraTears' registration tables (host functions by name: hle_impls; the NVN implementations:
 * nvni, nvnx), bounded by the linker's __start_ / __stop_ symbols, in the data segment after .data (as TSword's and
 * WSword PS5's are: left to the linker as orphans they landed in the RELRO segment's gap that the native converter fills
 * with the process parameters). */
SECTIONS {
    hle_impls : { KEEP(*(hle_impls)) }
    nvni : { KEEP(*(nvni)) }
    nvnx : { KEEP(*(nvnx)) }
} INSERT AFTER .data;
