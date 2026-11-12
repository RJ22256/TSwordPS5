// The WSword runtime's process end on the console (TSword PS5; ps5/tsword/build.sh links with --wrap=main,exit,_exit,
// atexit). A title ends by asking the shell to close it: exit() and _exit() kill it, and the console then reports a
// crash over a run that worked. So every way out - main returning, exit(), _exit() (plat_terminate) - runs the
// handlers the runtime registered (the GPU idle and the pipeline cache saved: runtime/nvn/vulkan/device.c), flushes,
// and closes the title. The CRT's own handlers (registered before main: static destructors) are left alone, as the
// PC's plat_terminate skips the library teardown while the game's threads still run.
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

int sceSystemServiceLoadExec(const char *path, const char *const *argv);
int sceKernelUsleep(unsigned microseconds);

int __real_main(int argc, char **argv, char **envp);
int __real_atexit(void (*fn)(void));

static atomic_bool g_in_main;
static void (*g_handlers[32])(void);
static atomic_int g_count;
static atomic_flag g_closing = ATOMIC_FLAG_INIT;

bool tsword_gamecode_load(void);
const char *tsword_gamecode_why(void);

int __wrap_main(int argc, char **argv, char **envp) {
    atomic_store(&g_in_main, true);
    // the game code first (gamecode.c): the runtime's main reads its layout before anything else. Without it the
    // runtime still starts and stops where it needs the game, with the reason on the TV.
    if (!tsword_gamecode_load()) fprintf(stderr, "[tsword] %s\n", tsword_gamecode_why());
    return __real_main(argc, argv, envp);
}

int __wrap_atexit(void (*fn)(void)) {
    if (!atomic_load(&g_in_main)) return __real_atexit(fn);  // (the CRT's)
    int i = atomic_fetch_add(&g_count, 1);
    if (i >= 32) return -1;
    g_handlers[i] = fn;
    return 0;
}

static _Thread_local bool t_closing;  // (this thread runs the handlers: one of them may end the process itself)
static atomic_flag g_asked = ATOMIC_FLAG_INIT;

static void ask_shell(int status) {
    if (atomic_flag_test_and_set(&g_asked)) return;
    fprintf(stderr, "[ps5] exit status %d: the shell closes the title\n", status);
    fflush(NULL);
    sceSystemServiceLoadExec("exit", NULL);
}

__attribute__((noreturn)) static void close_title(int status, bool handlers) {
    if (!atomic_flag_test_and_set(&g_closing)) {  // the first way out runs the handlers
        t_closing = true;
        if (handlers)
            for (int i = (atomic_load(&g_count) < 32 ? atomic_load(&g_count) : 32) - 1; i >= 0; i--) g_handlers[i]();
        ask_shell(status);
    } else if (t_closing) ask_shell(status);  // (a handler's own _exit: plat_terminate at the end of device.c's)
    for (;;) sceKernelUsleep(100000);  // (the shell closes the title asynchronously: never return)
}

void __wrap_exit(int status) { close_title(status, true); }
void __wrap__exit(int status) { close_title(status, false); }
void __wrap__Exit(int status) { close_title(status, false); }   // (TSword: rt_fatal ends with _Exit)
void catchReturnFromMain(int status) { close_title(status, true); }

// What the title's libc lacks that the runtime's dependencies name, and the platform layer doesn't replace (build.sh
// binds the ones it does: fork, isatty, getcwd...): path limits are FreeBSD's.
#include <errno.h>
#include <limits.h>
#include <unistd.h>
long pathconf(const char *path, int name) {
    (void)path;
    switch (name) {
    case _PC_NAME_MAX: return NAME_MAX;
    case _PC_PATH_MAX: return PATH_MAX;
    case _PC_PIPE_BUF: return PIPE_BUF;
    case _PC_LINK_MAX: return LINK_MAX;
    default: errno = EINVAL; return -1;
    }
}
