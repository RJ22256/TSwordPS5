/* ps5/tsword/sections.ld - the runtime's registration tables (its totk_ovr and totk_obs sections, bounded by the
 * linker's __start_ / __stop_ symbols) in the data segment, after .data, as WSword PS5's are: left to the linker as
 * orphans they landed in the RELRO segment's gap that the native converter fills with the process parameters ("LLVM
 * layout leaves no room for PS5 process parameters"). */
SECTIONS {
    totk_ovr : { KEEP(*(totk_ovr)) }
    totk_obs : { KEEP(*(totk_obs)) }
} INSERT AFTER .data;
