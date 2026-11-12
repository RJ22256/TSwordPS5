// (TSword PS5) The game code's side of the runtime: on PC tools/build_game.py links the runtime with the build of the
// player's game version - the lifted modules, their tables (modules.c: rt_layout, rt_linked_modules, the version and
// the ExeFS hashes) and the install side compiled with that version's headers (romfs.c, patches.c, ov_zstd.c,
// mouse_direct.c). On the console all of that is one ELF shared object, tsgame.so, which TSword Setup's PS5 export
// builds on the player's PC from their own game files (tools/build_ps5.py), loaded here before the runtime's main
// (elfload.c: a title can't dlopen what it brings):
//   - its functions the runtime calls are reached through jumps placed under their names (STUB below), aimed at the
//     library's own once it is loaded;
//   - its data the runtime reads (the layout, the ExeFS hashes, the paths and the version) is copied into the
//     definitions here, which the runtime's references bind to;
//   - what the library calls (the runtime's own functions and data, libc's, zstd's) it finds in the title's export
//     table (exports.py), by name.
// Without the library, or with one of another build, the runtime still starts and stops where it needs the game,
// saying why on the TV (tsword_fatal_notice).
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <ps5platform/exec.h>

#include "elfload.h"

#ifndef TSWORD_GAMECODE_ID
#error "TSWORD_GAMECODE_ID: what the PS5 export's game code must say it was built for (configure.py, gamecode_id.py)"
#endif
#define GAME_DIR "/app0"
#define GAME_LIB GAME_DIR "/tsgame.so"

extern const ElfExport g_tsword_exports[];
extern const size_t g_tsword_nexports;
void rt_fatal(const char *fmt, ...) __attribute__((noreturn));

// ---- the library's functions, under their own names in the title
// Each name is a jump through a pointer the loader sets, so the runtime's calls (any signature) reach the library's
// function with their arguments untouched. Unloaded, the pointer is no_game's.
#define GAME_FUNCTIONS(X)                                                                                              \
    X(rt_linked_modules)                                                                                               \
    X(fs_init) X(fs_trace) X(romfs_mount) X(romfs_open) X(romfs_read) X(romfs_size) X(romfs_close) X(romfs_handle)     \
    X(romfs_owns) X(romfs_entry_type) X(romfs_entry) X(romfs_file_count) X(romfs_image_path) X(romfs_overlay)          \
    X(romfs_overlay_count) X(romfs_patches_loaded) X(romfs_prompts) X(romfs_prompts_off) X(romfs_prompts_ready)        \
    X(romfs_shaped_prompts_ready) X(romfs_shaped_ready)                                                                \
    X(rt_patches_init) X(rt_patch_set) X(rt_patch_enabled) X(rt_patch_disable) X(rt_patch_switch) X(rt_patch_page)     \
    X(input_direct_mouse_active) X(input_direct_mouse_enabled) X(input_direct_mouse_shake)                             \
    X(input_direct_mouse_radial) X(input_direct_mouse_row)

static char g_why[640] = "the game code isn't loaded";
static bool g_loaded;

__attribute__((noreturn, used)) static void no_game(void) { rt_fatal("%s", g_why); }

#define STUB(n)                                                                                                        \
    void *tsg_p_##n __attribute__((used)) = (void *)no_game;                                                          \
    __asm__(".text\n.globl " #n "\n.type " #n ",@function\n.p2align 4\n" #n ":\n    jmp *tsg_p_" #n "(%rip)\n.size " #n \
            ", .-" #n "\n");
GAME_FUNCTIONS(STUB)

static const struct { const char *name; void **slot; } k_functions[] = {
#define SLOT(n) {#n, &tsg_p_##n},
    GAME_FUNCTIONS(SLOT)
#undef SLOT
};

// ---- the library's data, copied (the runtime's declarations: dispatch.h, host.h)
typedef struct { const char *name; uint64_t base, size; } Layout;
typedef struct { const char *file, *sha256; } FileHash;
enum { MAX_LAYOUT = 64, MAX_HASHES = 128, PATH_CAP = 512 };
// (until the library is loaded: the main module's place, which every version shares, and a size inside the window up
// to the kernel region at 0x7010000000, so mem_init reserves the module window before anything needs the game)
Layout tsg_layout[MAX_LAYOUT] __asm__("rt_layout") = {{"main", 0x7000000000ull, 0x8000000ull}};
int tsg_layout_count __asm__("rt_layout_count") = 1;
FileHash tsg_hashes[MAX_HASHES] __asm__("rt_exefs_sha256");
int tsg_hash_count __asm__("rt_exefs_sha256_count");
char tsg_exefs[PATH_CAP] __asm__("rt_default_exefs") = GAME_DIR "/exefs";
char tsg_romfs[PATH_CAP] __asm__("rt_default_romfs") = GAME_DIR "/romfs";
char tsg_profile[PATH_CAP] __asm__("rt_default_profile") = GAME_DIR "/profile";
int tsg_game_version __asm__("rt_game_version");
const char rt_port_version[] = TSWORD_PORT_VERSION;

// ---- loading

// Executable memory for the library, above every guest address (they are below 2^39, 0x8000000000: the runtime reserves
// its windows there after this), else anywhere the platform finds outside the GPU window.
static void *exec_alloc(size_t bytes) {
    struct ps5_exec_region rg;
    for (uintptr_t at = 0x9000000000ull; at < 0x20000000000ull; at += 0x1000000000ull) {
        struct ps5_exec_request rq = {.bytes = bytes, .address = at, .flags = PS5_EXEC_AT};
        if (!ps5_exec_alloc(&rq, &rg)) return rg.base;
    }
    struct ps5_exec_request rq = {.bytes = bytes};
    int r = ps5_exec_alloc(&rq, &rg);
    if (r) {
        fprintf(stderr, "[tsword] executable memory for the game code, %zu MB: %x\n", bytes >> 20, r);
        return NULL;
    }
    fprintf(stderr, "[tsword] the game code's executable memory is at %p, inside the guest's range\n", rg.base);
    return rg.base;
}

static bool read_text(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t k = fread(out, 1, n - 1, f);
    fclose(f);
    while (k && (out[k - 1] == '\n' || out[k - 1] == '\r' || out[k - 1] == ' ')) k--;
    out[k] = 0;
    return true;
}

static bool copy_data(const ElfImage *img) {
    const Layout *layout = elf_symbol(img, "rt_layout");
    const int *layout_count = elf_symbol(img, "rt_layout_count");
    const FileHash *hashes = elf_symbol(img, "rt_exefs_sha256");
    const int *hash_count = elf_symbol(img, "rt_exefs_sha256_count");
    const int *version = elf_symbol(img, "rt_game_version");
    if (!layout || !layout_count || !hashes || !hash_count || !version) {
        snprintf(g_why, sizeof g_why, "%s has no module tables: not TSword's game code", GAME_LIB);
        return false;
    }
    if (*layout_count < 1 || *layout_count > MAX_LAYOUT || *hash_count < 0 || *hash_count > MAX_HASHES) {
        snprintf(g_why, sizeof g_why, "%s: %d modules and %d ExeFS hashes, more than TSword holds", GAME_LIB,
                 *layout_count, *hash_count);
        return false;
    }
    memcpy(tsg_layout, layout, (size_t)*layout_count * sizeof *layout);
    tsg_layout_count = *layout_count;
    memcpy(tsg_hashes, hashes, (size_t)*hash_count * sizeof *hashes);
    tsg_hash_count = *hash_count;
    tsg_game_version = *version;
    // (the paths: the title folder's, as the PC's portable layout has them beside the exe; runtime/main.c
    // portable_paths finds them there either way)
    return true;
}

// Before the runtime's main (ps5_glue.c __wrap_main). false: g_why says what's missing.
bool tsword_gamecode_load(void) {
    char id[256];
    FILE *probe = fopen(GAME_LIB, "rb");
    if (!probe) {
        snprintf(g_why, sizeof g_why,
                 "No game code in the title folder yet (tsgame.so). On your PC: run TSword Setup with your game files, "
                 "then its PS5 export, and copy the PS5 folder's contents here.");
        return false;
    }
    fclose(probe);
    if (!read_text(GAME_LIB ".id", id, sizeof id) || strcmp(id, TSWORD_GAMECODE_ID)) {
        snprintf(g_why, sizeof g_why,
                 "The game code (tsgame.so) was built by another version of TSword Setup (%.24s, this title needs "
                 "%.24s). Run the PS5 export of the TSword Setup made for this release and copy it again.",
                 id[0] ? id : "no tsgame.so.id", TSWORD_GAMECODE_ID);
        return false;
    }
    static ElfImage img;
    char why[512];
    if (!elf_load(GAME_LIB, g_tsword_exports, g_tsword_nexports, exec_alloc, &img, why, sizeof why)) {
        snprintf(g_why, sizeof g_why, "Loading the game code failed: %s", why);
        return false;
    }
    const char *lib_id = elf_symbol(&img, "tsword_gamecode_id");
    if (!lib_id || strcmp(lib_id, TSWORD_GAMECODE_ID)) {
        snprintf(g_why, sizeof g_why, "%s was built for another TSword release (%.24s)", GAME_LIB,
                 lib_id ? lib_id : "no id");
        return false;
    }
    for (size_t i = 0; i < sizeof k_functions / sizeof *k_functions; i++) {
        void *f = elf_symbol(&img, k_functions[i].name);
        if (!f) {
            snprintf(g_why, sizeof g_why, "%s lacks %s: not TSword's game code", GAME_LIB, k_functions[i].name);
            return false;
        }
        *k_functions[i].slot = f;
    }
    if (!copy_data(&img)) return false;
    g_loaded = true;
    fprintf(stderr, "[tsword] game code: %s, %llu MB at %p, game %d.%d.%d, %d modules\n", GAME_LIB,
            (unsigned long long)(img.size >> 20), (void *)img.base, tsg_game_version / 100, tsg_game_version / 10 % 10,
            tsg_game_version % 10, tsg_layout_count);
    return true;
}

const char *tsword_gamecode_why(void) { return g_why; }

// Every fatal stop of the runtime (dispatch.c rt_fatal, ps5/tsword-patches/0002), on the TV before the title closes:
// the console's notification (the SDL subset's message box), and time to read it.
void tsword_fatal_notice(const char *msg) {
    char text[900];
    if (!g_loaded) snprintf(text, sizeof text, "%s", g_why);
    else snprintf(text, sizeof text, "Stopped: %s. (logs/totk.log in the title folder has the details.)", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TSword", text, NULL);
    SDL_Delay(8000);
}
