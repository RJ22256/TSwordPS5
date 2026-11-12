// (TSword PS5, milestone 3) The game code's side of the runtime, until there is game code: on PC tools/build_game.py
// links the runtime with the build of the player's game version - the lifted modules, their tables (modules.c:
// rt_layout, rt_linked_modules, the version and the ExeFS hashes) and the install side compiled with that version's
// headers (romfs.c, patches.c, ov_zstd.c, mouse_direct.c). On the console that is the PS5 export's game code
// (milestone 4), loaded at start; these stand in for it meanwhile, so the title links and the runtime starts up to
// where it needs the game - and says so in its log.
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL3/SDL.h>

#include "dispatch.h"
#include "fs.h"
#include "host.h"
#include "input.h"

#define NO_GAME "TSword: no game code yet - the PS5 export (milestone 4) brings it from your PC"

__attribute__((noreturn)) static void no_game(const char *where) {
    fprintf(stderr, "%s (%s)\n", NO_GAME, where);
    rt_fatal("%s", NO_GAME);   // (tsword_fatal_notice puts it on the TV)
}

// Every fatal stop of the runtime (dispatch.c rt_fatal, ps5/tsword-patches/0002), on the TV before the title closes:
// the console's notification (the SDL subset's message box), and time to read it. Without game code - the loader's
// first look at the ExeFS, or a stand-in above - it says so plainly.
void tsword_fatal_notice(const char *msg) {
    char text[640];
    int no_code = rt_layout_count <= 1 && rt_exefs_sha256_count == 0;
    if (no_code)
        snprintf(text, sizeof text, "The runtime started, then stopped where it needs the game: no game code yet - the PS5 "
                                    "export (milestone 4) brings it from your PC. (logs/totk.log has the details.)");
    else
        snprintf(text, sizeof text, "Stopped: %s. (logs/totk.log in the title folder has the details.)", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TSword", text, NULL);
    SDL_Delay(6000);
}

// modules.c's
// (the main module's place, which every version shares, and a size inside the window up to the kernel region at
// 0x7010000000: mem_init reserves the module window from it before anything needs the game)
const RtLayoutEntry rt_layout[1] = {{"main", 0x7000000000ull, 0x8000000ull}};
const int rt_layout_count = 1;
const RtFileHash rt_exefs_sha256[1];
const int rt_exefs_sha256_count = 0;
const char rt_default_exefs[] = "", rt_default_romfs[] = "", rt_default_profile[] = "";
const int rt_game_version = 0;
const char rt_port_version[] = TSWORD_PORT_VERSION;
int rt_linked_modules(RtModule *out, int max) {
    (void)out, (void)max;
    no_game("the game's modules");
}

// romfs.c's: the game files
void fs_init(void) { no_game("the game files"); }
void fs_trace(const char *what, const char *path) { (void)what, (void)path; }
uint32_t romfs_mount(const char *name) { (void)name; no_game("the RomFS"); }
uint32_t romfs_open(uint64_t *out, const char *path, int mode) { (void)out, (void)path, (void)mode; no_game("a RomFS file"); }
uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    (void)out, (void)h, (void)offset, (void)buffer, (void)size;
    no_game("a RomFS read");
}
uint32_t romfs_size(int64_t *out, uint64_t h) { (void)out, (void)h; no_game("a RomFS size"); }
void romfs_close(uint64_t h) { (void)h; }
int romfs_handle(uint64_t h) { (void)h; return 0; }
int romfs_owns(const char *path) { (void)path; return 0; }
uint32_t romfs_entry_type(int *out, const char *path) { (void)out, (void)path; no_game("a RomFS entry"); }
const char *romfs_entry(uint64_t i, uint64_t *offset, uint64_t *size) { (void)i, (void)offset, (void)size; return NULL; }
uint64_t romfs_file_count(void) { return 0; }
const char *romfs_image_path(void) { return ""; }
const char *romfs_overlay(size_t k, const char **file, uint64_t *size, int *is_added) {
    (void)k, (void)file, (void)size, (void)is_added;
    return NULL;
}
size_t romfs_overlay_count(void) { return 0; }
int romfs_patches_loaded(void) { return 0; }
const char *romfs_prompts(void) { return ""; }
int romfs_prompts_off(const char *folder) { (void)folder; return 1; }
int romfs_prompts_ready(const char *folder) { (void)folder; return 0; }
int romfs_shaped_prompts_ready(const char *folder, unsigned w, unsigned h) { (void)folder, (void)w, (void)h; return 0; }
int romfs_shaped_ready(unsigned w, unsigned h) { (void)w, (void)h; return 0; }

// patches.c's: the version's patch sets
void rt_patches_init(void) {}
int rt_patch_set(const char *name) { (void)name; return -1; }
int rt_patch_enabled(int set) { (void)set; return 0; }
void rt_patch_disable(int set) { (void)set; }
void rt_patch_switch(int set, int on) { (void)set, (void)on; }
void *rt_patch_page(void) { return NULL; }

// mouse_direct.c's: no mouse on the console
int input_direct_mouse_active(void) { return 0; }
int input_direct_mouse_enabled(void) { return 0; }
int input_direct_mouse_shake(void) { return 0; }
void input_direct_mouse_radial(int32_t dx, int32_t dy, float sensitivity, float cursor[2], int32_t out[2]) {
    (void)dx, (void)dy, (void)sensitivity, (void)cursor;
    out[0] = out[1] = 0;
}
void input_direct_mouse_row(int32_t dx, float sensitivity, uint64_t now, InputMouseRow *row, int32_t out[2]) {
    (void)dx, (void)sensitivity, (void)now, (void)row;
    out[0] = out[1] = 0;
}
