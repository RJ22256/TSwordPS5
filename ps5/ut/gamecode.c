// (TSword PS5) The game code and the game files, for UltraTears on the console. On a PC tools/build_game.py links
// the runtime with the lifted modules (data/generated: main, subsdk0, sdk, each with its table of functions); on the
// console the lifted code is one ELF shared object, tsgame.so, which the PC's PS5 export builds from the player's own
// dump (tools/build_ps5.py), loaded here before the runtime's main (elfload.c: a title can't dlopen what it brings):
//   - the runtime reads its tables by name (runtime/dispatch.c, tsword_game_symbol);
//   - what the library calls (the runtime's functions and data, libc's) it finds in the title's export table
//     (ps5/ut/exports.py), by name.
// Without the library, or with one of another build, the runtime still starts and stops where it needs the game,
// saying why on the TV.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <ps5platform/exec.h>

#include "elfload.h"

#ifndef TSWORD_GAMECODE_ID
#error "TSWORD_GAMECODE_ID: what the PS5 export's game code must say it was built for (configure.py)"
#endif
#define TITLE_DIR "/app0"

extern const ElfExport g_tsword_exports[];
extern const size_t g_tsword_nexports;

static char g_lib[700] = TITLE_DIR "/tsgame.so";
static char g_why[900] = "The game code isn't loaded.";
static bool g_loaded;
static ElfImage g_img;

const void *tsword_game_symbol(const char *name) { return g_loaded ? elf_symbol(&g_img, name) : NULL; }

// ---- the game files: the title folder, or an external drive
// The PC's PS5 export folder - data/ (exefs, romfs, shaders, textures, fonts) and tsgame.so - is copied to the title
// folder, a USB drive or extended storage. In order, the first with data/exefs/main is used:
//   1. the folder named by the first line of game-files.txt in the title folder (a full path)
//   2. the title folder itself
//   3. on each USB drive (/mnt/usb0 to usb7) and extended storage (/mnt/ext0 to ext2): TSword/, PPSA99778/,
//      UltraTears/, PS5/ or the drive's top
// The runtime is pointed at it (its ultratears.cfg names: exefs, romfs, shaderpack, textures, fonts), and tsgame.so is
// taken from it when the title folder has none. The settings (ultratears.cfg), the saves (userdata/), the log and the
// console's own compiled shaders stay in the title folder.
static bool exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool has_game(const char *dir) {
    char p[800];
    snprintf(p, sizeof p, "%s/data/exefs/main", dir);
    return exists(p);
}

static void use_game_dir(const char *dir) {
    char p[800];
    snprintf(p, sizeof p, "%s/data/exefs", dir), setenv("ULTRS_EXEFS", p, 1);
    snprintf(p, sizeof p, "%s/data/romfs", dir), setenv("TOTK_ROMFS", p, 0);
    snprintf(p, sizeof p, "%s/data/shaders/translated.pack", dir);
    if (exists(p)) setenv("TOTK_SHADER_PACK", p, 0);
    snprintf(p, sizeof p, "%s/data/shaders/translated", dir);
    if (exists(p)) setenv("TOTK_SHADER_DIR", p, 0);
    snprintf(p, sizeof p, "%s/data/textures", dir), setenv("TOTK_BC7_DIR", p, 0);
    snprintf(p, sizeof p, "%s/data/fonts", dir), setenv("TOTK_SHARED_FONTS", p, 0);
    snprintf(p, sizeof p, "%s/tsgame.so", dir);
    if (!exists(TITLE_DIR "/tsgame.so") && exists(p)) snprintf(g_lib, sizeof g_lib, "%s", p);
}

// (0.4.5) A copy of ultratears.log beside the game files on the drive, brought up to date every second and flushed
// to the drive: after a freeze the drive can go straight to a PC (the title folder's log needs the console up again).
// TOTK_LOG_MIRROR=0: none.
static char g_mirror_dst[640];
static void *log_mirror_thread(void *unused) {
    (void)unused;
    off_t done = 0;
    char buf[65536];
    for (;;) {
        int in = open(TITLE_DIR "/ultratears.log", O_RDONLY), out = open(g_mirror_dst, O_WRONLY | O_CREAT, 0644);
        if (in >= 0 && out >= 0) {
            ssize_t n;
            bool wrote = false;
            while ((n = pread(in, buf, sizeof buf, done)) > 0) {
                if (pwrite(out, buf, (size_t)n, done) != n) break;
                done += n, wrote = true;
            }
            static int passes;
            if (wrote && ++passes % 5 == 0) fsync(out);   // (0.4.10) onto the drive every 5 s, not every second
        }
        if (in >= 0) close(in);
        if (out >= 0) close(out);
        sleep(1);
    }
    return NULL;
}
static void log_mirror_start(const char *dir) {
    const char *e = getenv("TOTK_LOG_MIRROR");
    if (e && *e == '0') return;
    char prev[660];
    snprintf(g_mirror_dst, sizeof g_mirror_dst, "%s/ultratears.log", dir);
    snprintf(prev, sizeof prev, "%s/ultratears-previous.log", dir);
    rename(g_mirror_dst, prev);
    pthread_t t;
    if (pthread_create(&t, NULL, log_mirror_thread, NULL) == 0) {
        pthread_detach(t);
        fprintf(stderr, "[tsword] the log is copied to %s every second too (TOTK_LOG_MIRROR=0: not)\n", g_mirror_dst);
    }
}

// (0.4.7) Safe mode: the console's defaults while TSword stops after its first frame. Every optional thread, speed-up
// and risky path at its plainest; ultratears.cfg can set any of them otherwise (a line naming it wins), and
// TOTK_SAFE_MODE=0 there leaves them all at UltraTears' own defaults.
static const char *const k_safe[][2] = {
    {"TOTK_FREE_CORES", "1"},          // no virtual cores: game threads are ordinary threads (no core hand-overs)
    {"TOTK_ASYNC_REPLAY", "0"},        // graphics replayed on the game's thread, no translation thread
    {"TOTK_VK_RECORD_THREADS", "1"},   // one recording thread
    {"TOTK_VK_PARTIAL_SUBMIT", "0"},   // each submission in one call
    {"TOTK_VK_MEM_READY", "0"},        // no spare memory made ahead on its own thread
    {"TOTK_SHADER_AHEAD", "0"},        // no shader compiling ahead on its own thread
    {"TOTK_TEX_PREFETCH", "0"},        // no texture prefetching
    {"TOTK_WB_FENCE", "0"},            // GPU-drawn images written back the plain way
    {"TOTK_POOL_EARLY_RELEASE", "0"},  // memory pools kept until the game releases them
    {"TOTK_VK_ROBUST", "2"},           // out-of-range GPU buffer reads give 0 instead of faulting
    {"TOTK_VK_CHECKPOINTS", "1"},      // (0.4.13) GPU markers: a stop names the GPU commands it ran last
    // (0.4.16) game memory imported for the GPU, as WSword does on this driver, instead of copies of every pool: without
    // the write watch the copies cannot tell the game's newer bytes from the GPU's, and their copy back overwrote the
    // game's heap (0.4.15: both runs crashed inside the game's allocator, a pointer 0x00ffff0000000000 / 0x8); and the
    // copies took ~1 GiB more memory
    {"TOTK_POOL_IMPORT", "1"},
};
static bool cfg_names(const char *cfg, const char *key) {   // a line "KEY=..." (not a comment)
    size_t n = strlen(key);
    for (const char *l = cfg; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        while (*l == ' ' || *l == '\t') l++;
        if (!strncmp(l, key, n) && (l[n] == '=' || l[n] == ' ')) return true;
    }
    return false;
}
static void safe_mode(void) {
    static char cfg[16384];
    cfg[0] = 0;
    FILE *f = fopen(TITLE_DIR "/ultratears.cfg", "r");
    if (f) { size_t n = fread(cfg, 1, sizeof cfg - 1, f); cfg[n] = 0; fclose(f); }
    if (cfg_names(cfg, "TOTK_SAFE_MODE")) {
        const char *v = strstr(cfg, "TOTK_SAFE_MODE");
        v = strchr(v, '=');
        if (v && v[1] == '0') { fprintf(stderr, "[tsword] safe mode: off (TOTK_SAFE_MODE=0)\n"); return; }
    }
    char kept[512] = "";
    int set = 0;
    for (unsigned i = 0; i < sizeof k_safe / sizeof *k_safe; i++) {
        if (cfg_names(cfg, k_safe[i][0]) || getenv(k_safe[i][0])) {
            size_t l = strlen(kept);
            snprintf(kept + l, sizeof kept - l, "%s%s", l ? ", " : "", k_safe[i][0]);
            continue;
        }
        setenv(k_safe[i][0], k_safe[i][1], 1), set++;
    }
    fprintf(stderr, "[tsword] safe mode: %d settings at their plainest (TOTK_SAFE_MODE=0 in ultratears.cfg: off)%s%s\n", set,
            kept[0] ? "; ultratears.cfg's own: " : "", kept);
}

static void find_game_files(void) {
    // what stays in the title folder: the console's own compiled shaders (its driver's, not the PC's), the saves
    setenv("TOTK_SHADER_BIN", TITLE_DIR "/data/shaders/compiled-ps5.bin", 0);
    // (0.4.9) its folder made first: without it the binaries could not be written ("cannot write ...: shaders are
    // compiled every run"), so every start compiled the game's thousands of startup shaders again
    mkdir(TITLE_DIR "/data", 0755);
    mkdir(TITLE_DIR "/data/shaders", 0755);
    setenv("TOTK_PROFILE_DIR", TITLE_DIR "/userdata", 0);
    FILE *f = fopen(TITLE_DIR "/game-files.txt", "r");
    if (f) {
        char line[600] = "";
        if (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            size_t k = strlen(line);
            while (k > 1 && line[k - 1] == '/') line[--k] = 0;
            if (line[0] && has_game(line)) {
                fclose(f);
                use_game_dir(line);
                fprintf(stderr, "[tsword] game files: %s (game-files.txt)\n", line);
                return;
            }
            fprintf(stderr, "[tsword] game-files.txt names %s, which has no data/exefs/main\n", line);
        }
        fclose(f);
    }
    if (has_game(TITLE_DIR)) {
        use_game_dir(TITLE_DIR);
        fprintf(stderr, "[tsword] game files: the title folder\n");
        return;
    }
    static const char *const drives[] = {"/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/usb4", "/mnt/usb5",
                                         "/mnt/usb6", "/mnt/usb7", "/mnt/ext0", "/mnt/ext1", "/mnt/ext2"};
    static const char *const sub[] = {"TSword", "PPSA99778", "UltraTears", "PS5", ""};
    for (size_t d = 0; d < sizeof drives / sizeof *drives; d++) {
        if (!exists(drives[d])) continue;
        for (size_t i = 0; i < sizeof sub / sizeof *sub; i++) {
            char p[600];
            snprintf(p, sizeof p, "%s%s%s", drives[d], sub[i][0] ? "/" : "", sub[i]);
            if (has_game(p)) {
                use_game_dir(p);
                fprintf(stderr, "[tsword] game files: %s (an external drive)\n", p);
                log_mirror_start(p);
                return;
            }
        }
        fprintf(stderr, "[tsword] %s: no TSword folder with data/exefs/main on it\n", drives[d]);
    }
    fprintf(stderr, "[tsword] game files: none found (the title folder, or TSword/ on a USB or extended storage drive)\n");
}

// ---- loading
// Executable memory for the library, above every guest address (below 2^39: the runtime reserves its window there
// after this), else anywhere the platform finds outside the GPU window.
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

// Before the runtime's main (ps5_glue.c __wrap_main). false: g_why says what's missing.
bool tsword_gamecode_load(void) {
    safe_mode();
    find_game_files();
    if (!exists(g_lib)) {
        snprintf(g_why, sizeof g_why,
                 "No game code yet (tsgame.so). On your PC: run the TSword PS5 export with your UltraTears game folder, "
                 "then copy its PS5 folder to TSword/ on a USB drive (or into the title folder).");
        return false;
    }
    char idpath[760], id[256] = "";
    snprintf(idpath, sizeof idpath, "%s.id", g_lib);
    if (!read_text(idpath, id, sizeof id) || strcmp(id, TSWORD_GAMECODE_ID)) {
        snprintf(g_why, sizeof g_why,
                 "The game code (tsgame.so) was made for another TSword release (%.24s; this one needs %.24s). Run "
                 "the PS5 export of this release again and copy it over.", id[0] ? id : "no tsgame.so.id",
                 TSWORD_GAMECODE_ID);
        return false;
    }
    char why[512];
    if (!elf_load(g_lib, g_tsword_exports, g_tsword_nexports, exec_alloc, &g_img, why, sizeof why)) {
        snprintf(g_why, sizeof g_why, "Loading the game code failed: %s", why);
        return false;
    }
    const char *lib_id = elf_symbol(&g_img, "tsword_gamecode_id");
    if (!lib_id || strcmp(lib_id, TSWORD_GAMECODE_ID)) {
        snprintf(g_why, sizeof g_why, "%s was built for another TSword release (%.24s)", g_lib, lib_id ? lib_id : "no id");
        return false;
    }
    g_loaded = true;
    fprintf(stderr, "[tsword] game code: %s, %llu MB at %p\n", g_lib, (unsigned long long)(g_img.size >> 20),
            (void *)g_img.base);
    return true;
}

const char *tsword_gamecode_why(void) { return g_why; }

// A fatal stop (runtime/platform/ps5.c plat_fatal_notice): the reason as the console's notification - the last FATAL
// line of the log, or why the game code isn't there - then the title closes through the shell (ps5_glue.c's exit).
void tsword_fatal_notice(void) {
    char text[1000], line[600] = "";
    FILE *f = fopen(TITLE_DIR "/ultratears.log", "rb");
    if (f) {
        char buf[8192];
        long size = (fseek(f, 0, SEEK_END), ftell(f));
        long from = size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0;
        fseek(f, from, SEEK_SET);
        size_t k = fread(buf, 1, sizeof buf - 1, f);
        buf[k] = 0;
        fclose(f);
        char *last = NULL;
        for (char *p = strstr(buf, "FATAL"); p; p = strstr(p + 1, "FATAL")) last = p;
        if (last) {
            size_t n = strcspn(last, "\r\n");
            snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line - 1 ? n : sizeof line - 1), last);
        }
    }
    if (!g_loaded) snprintf(text, sizeof text, "%s", g_why);
    else snprintf(text, sizeof text, "Stopped: %s (ultratears.log in the title folder has the details.)",
                  line[0] ? line : "an error");
    fprintf(stderr, "[ps5] message: TSword: %s\n", text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TSword", text, NULL);
    SDL_Delay(8000);
    exit(70);
}
