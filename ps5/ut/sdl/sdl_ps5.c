// The SDL3 the runtime calls, on the console: WSword PS5's subset (its 110 functions), with what TotK Recomp's runtime
// calls besides (TSword PS5: the end of this file),
// against SDL 3.4's own headers (the runtime's types and constants unchanged), over the console's services:
//   the window      one, fullscreen: the TV through VK_KHR_display (RADV's display, SDL_Vulkan_CreateSurface)
//   the gamepad     the signed-in user's DualSense (scePad): buttons, sticks, triggers, motion, rumble, as SDL's
//                   events and state; the touch pad's click is Back (the Create button belongs to the system)
//   audio           every stream mixed into one 48 kHz stereo output (sceAudioOut), each converted and resampled
//   the rest        properties, time, events, a clipboard, message boxes as notifications; no mouse, no keyboard
//                   (a USB keyboard is for later), no dialogs (none on the console: they report a cancel)
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>  // (before SDL_vulkan.h: the whole API, KHR_display included)
#include <SDL3/SDL_vulkan.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5platform/memory.h>

// ---- the console's services (their libraries are the SDK's; declared as the template's platform.c does)
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *user_id);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t port_type, int32_t index, const void *params);
int scePadRead(int32_t handle, void *samples, int32_t capacity);
int scePadClose(int32_t handle);
int scePadSetVibrationMode(int32_t handle, int32_t mode);
int scePadSetVibration(int32_t handle, const void *vibration);
int scePadSetMotionSensorState(int32_t handle, bool enable);
int scePadSetLightBar(int32_t handle, const void *color);
int scePadSetTriggerEffect(int32_t handle, const void *param);
int sceAudioOutClose(int32_t port);
int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t grain, uint32_t rate, uint32_t format);
int sceAudioOutOutput(int32_t port, const void *samples);
int sceSystemServiceHideSplashScreen(void);
int sceKernelSendNotificationRequest(int device, void *request, size_t size, int blocking);

// ---- errors
static _Thread_local char t_error[256];

static bool fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t_error, sizeof t_error, fmt, ap);
    va_end(ap);
    return false;
}

const char *SDL_GetError(void) { return t_error; }

// ---- memory, strings, hints, init
void SDL_free(void *mem) { free(mem); }
char *SDL_strdup(const char *s) { return s ? strdup(s) : NULL; }
bool SDL_SetHint(const char *name, const char *value) { (void)name, (void)value; return true; }

static void pad_start(void);
static bool audio_start_device(void);

bool SDL_InitSubSystem(SDL_InitFlags flags) {
    if (flags & SDL_INIT_GAMEPAD) pad_start();
    if (flags & SDL_INIT_AUDIO) return audio_start_device();
    return true;
}
bool SDL_Init(SDL_InitFlags flags) { return SDL_InitSubSystem(flags); }

// ---- time
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}
static uint64_t g_t0;
static uint64_t ticks_ns(void) {
    if (!g_t0) g_t0 = now_ns();
    return now_ns() - g_t0;
}
Uint64 SDL_GetTicksNS(void) { return ticks_ns(); }
Uint64 SDL_GetTicks(void) { return ticks_ns() / 1000000u; }
Uint64 SDL_GetPerformanceCounter(void) { return now_ns(); }
Uint64 SDL_GetPerformanceFrequency(void) { return 1000000000u; }
static void sleep_ns(uint64_t ns) {
    struct timespec ts = {.tv_sec = (time_t)(ns / 1000000000u), .tv_nsec = (long)(ns % 1000000000u)};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}
void SDL_Delay(Uint32 ms) { sleep_ns((uint64_t)ms * 1000000u); }
void SDL_DelayPrecise(Uint64 ns) {
    uint64_t end = now_ns() + ns;
    if (ns > 1000000) sleep_ns(ns - 1000000);  // (the last millisecond spun: the scheduler's tick is coarser)
    while (now_ns() < end) __builtin_ia32_pause();
}

// ---- the machine
int SDL_GetNumLogicalCPUCores(void) {
    long ps5_sysconf(int name);
    long n = ps5_sysconf(_SC_NPROCESSORS_ONLN);  // (the CPUs a title's threads may run on: 13 of the 16)
    return n > 0 ? (int)n : 8;
}
int SDL_GetSystemRAM(void) {  // (direct memory, the title's own: what the runtime sizes its caches by)
    struct ps5_memory_stats st;
    if (ps5_memory_query(&st)) return (int)(st.total_bytes >> 20);
    return 12288;
}
SDL_PowerState SDL_GetPowerInfo(int *seconds, int *percent) {
    if (seconds) *seconds = -1;
    if (percent) *percent = -1;
    return SDL_POWERSTATE_NO_BATTERY;
}

// ---- properties: a few small key / value lists
typedef struct {
    char name[96];
    int type;  // SDL_PropertyType
    union { void *p; char *s; Sint64 n; float f; bool b; } v;
} Prop;
typedef struct { bool used; int n; Prop items[32]; } PropSet;
static PropSet g_props[64];
static pthread_mutex_t g_props_lock = PTHREAD_MUTEX_INITIALIZER;

SDL_PropertiesID SDL_CreateProperties(void) {
    pthread_mutex_lock(&g_props_lock);
    for (int i = 1; i < 64; i++)
        if (!g_props[i].used) {
            memset(&g_props[i], 0, sizeof g_props[i]);
            g_props[i].used = true;
            pthread_mutex_unlock(&g_props_lock);
            return (SDL_PropertiesID)i;
        }
    pthread_mutex_unlock(&g_props_lock);
    fail("out of properties");
    return 0;
}
void SDL_DestroyProperties(SDL_PropertiesID id) {
    if (id <= 0 || id >= 64) return;
    pthread_mutex_lock(&g_props_lock);
    for (int i = 0; i < g_props[id].n; i++)
        if (g_props[id].items[i].type == SDL_PROPERTY_TYPE_STRING) free(g_props[id].items[i].v.s);
    g_props[id].used = false;
    pthread_mutex_unlock(&g_props_lock);
}
static Prop *prop_find(SDL_PropertiesID id, const char *name, bool make) {  // (with the lock)
    if (id <= 0 || id >= 64 || !g_props[id].used || !name) return NULL;
    PropSet *s = &g_props[id];
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->items[i].name, name)) return &s->items[i];
    if (!make || s->n == 32) return NULL;
    Prop *p = &s->items[s->n++];
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    return p;
}
static bool prop_set(SDL_PropertiesID id, const char *name, int type, const void *value) {
    pthread_mutex_lock(&g_props_lock);
    Prop *p = prop_find(id, name, true);
    if (p) {
        if (p->type == SDL_PROPERTY_TYPE_STRING) free(p->v.s);
        p->type = type;
        switch (type) {
        case SDL_PROPERTY_TYPE_STRING: p->v.s = strdup(value ? (const char *)value : ""); break;
        case SDL_PROPERTY_TYPE_NUMBER: p->v.n = *(const Sint64 *)value; break;
        case SDL_PROPERTY_TYPE_FLOAT: p->v.f = *(const float *)value; break;
        case SDL_PROPERTY_TYPE_BOOLEAN: p->v.b = *(const bool *)value; break;
        default: p->v.p = (void *)value; break;
        }
    }
    pthread_mutex_unlock(&g_props_lock);
    return p != NULL;
}
bool SDL_SetNumberProperty(SDL_PropertiesID id, const char *name, Sint64 v) { return prop_set(id, name, SDL_PROPERTY_TYPE_NUMBER, &v); }
bool SDL_SetStringProperty(SDL_PropertiesID id, const char *name, const char *v) { return prop_set(id, name, SDL_PROPERTY_TYPE_STRING, v); }
bool SDL_SetBooleanProperty(SDL_PropertiesID id, const char *name, bool v) { return prop_set(id, name, SDL_PROPERTY_TYPE_BOOLEAN, &v); }
bool SDL_GetBooleanProperty(SDL_PropertiesID id, const char *name, bool def) {
    pthread_mutex_lock(&g_props_lock);
    Prop *p = prop_find(id, name, false);
    bool v = p && p->type == SDL_PROPERTY_TYPE_BOOLEAN ? p->v.b : def;
    pthread_mutex_unlock(&g_props_lock);
    return v;
}
float SDL_GetFloatProperty(SDL_PropertiesID id, const char *name, float def) {
    pthread_mutex_lock(&g_props_lock);
    Prop *p = prop_find(id, name, false);
    float v = p && p->type == SDL_PROPERTY_TYPE_FLOAT ? p->v.f : def;
    pthread_mutex_unlock(&g_props_lock);
    return v;
}
void *SDL_GetPointerProperty(SDL_PropertiesID id, const char *name, void *def) {
    pthread_mutex_lock(&g_props_lock);
    Prop *p = prop_find(id, name, false);
    void *v = p && p->type == SDL_PROPERTY_TYPE_POINTER ? p->v.p : def;
    pthread_mutex_unlock(&g_props_lock);
    return v;
}

// ---- events
static SDL_Event g_events[1024];
static unsigned g_ev_head, g_ev_tail;  // (head == tail: empty)
static pthread_mutex_t g_ev_lock = PTHREAD_MUTEX_INITIALIZER;

static void push(SDL_Event *e) {
    if (!e->common.timestamp) e->common.timestamp = ticks_ns();
    pthread_mutex_lock(&g_ev_lock);
    unsigned next = (g_ev_tail + 1) % 1024;
    if (next != g_ev_head) { g_events[g_ev_tail] = *e; g_ev_tail = next; }  // (full: dropped, as SDL drops)
    pthread_mutex_unlock(&g_ev_lock);
}
bool SDL_PushEvent(SDL_Event *e) { push(e); return true; }

static void pump(void);

bool SDL_PollEvent(SDL_Event *e) {
    pump();
    pthread_mutex_lock(&g_ev_lock);
    bool any = g_ev_head != g_ev_tail;
    if (any) {
        if (e) { *e = g_events[g_ev_head]; g_ev_head = (g_ev_head + 1) % 1024; }
    }
    pthread_mutex_unlock(&g_ev_lock);
    return any;
}
bool SDL_WaitEventTimeout(SDL_Event *e, Sint32 timeout_ms) {
    uint64_t end = ticks_ns() + (uint64_t)(timeout_ms < 0 ? 0 : timeout_ms) * 1000000u;
    for (;;) {
        if (SDL_PollEvent(e)) return true;
        if (timeout_ms >= 0 && ticks_ns() >= end) return false;
        sleep_ns(1000000);
    }
}

// ---- the display and the window
// One display, the TV. Its mode until the surface is made: WSWORD_PS5_OUTPUT (e.g. 3840x2160), else 1920x1080 at
// 60 Hz; SDL_Vulkan_CreateSurface takes the display's mode closest to it and the window follows.
static SDL_DisplayMode g_mode = {.displayID = 1, .format = SDL_PIXELFORMAT_XRGB8888, .w = 1920, .h = 1080,
                                 .pixel_density = 1.0f, .refresh_rate = 60.0f, .refresh_rate_numerator = 60,
                                 .refresh_rate_denominator = 1};
static bool g_mode_read;

static void mode_init(void) {
    if (g_mode_read) return;
    g_mode_read = true;
    const char *o = getenv("WSWORD_PS5_OUTPUT");
    int w, h;
    if (o && sscanf(o, "%dx%d", &w, &h) == 2 && w >= 640 && h >= 360) g_mode.w = w, g_mode.h = h;
}

struct SDL_Window {
    int w, h;
    SDL_WindowFlags flags;
    SDL_PropertiesID props;
    SDL_Rect mouse_rect;
};
static SDL_Window g_window;
static bool g_window_made;

SDL_DisplayID *SDL_GetDisplays(int *count) {
    SDL_DisplayID *d = calloc(2, sizeof *d);
    if (d) d[0] = 1;
    if (count) *count = 1;
    return d;
}
SDL_DisplayID SDL_GetPrimaryDisplay(void) { return 1; }
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *w) { (void)w; return 1; }
const char *SDL_GetDisplayName(SDL_DisplayID id) { (void)id; return "TV"; }
const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID id) { (void)id; mode_init(); return &g_mode; }
const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID id) { (void)id; mode_init(); return &g_mode; }
SDL_DisplayMode **SDL_GetFullscreenDisplayModes(SDL_DisplayID id, int *count) {
    (void)id;
    mode_init();
    SDL_DisplayMode **l = calloc(1, 2 * sizeof *l + sizeof **l);  // (one allocation: the list, then the mode)
    if (!l) { if (count) *count = 0; return NULL; }
    l[0] = (SDL_DisplayMode *)(l + 2);
    *l[0] = g_mode;
    if (count) *count = 1;
    return l;
}
bool SDL_GetDisplayUsableBounds(SDL_DisplayID id, SDL_Rect *r) {
    (void)id;
    mode_init();
    if (r) *r = (SDL_Rect){0, 0, g_mode.w, g_mode.h};
    return true;
}
const char *SDL_GetCurrentVideoDriver(void) { return "ps5"; }

SDL_Window *SDL_CreateWindowWithProperties(SDL_PropertiesID props) {
    (void)props;
    mode_init();
    if (g_window_made) { fail("one window on the console"); return NULL; }
    g_window = (SDL_Window){.w = g_mode.w, .h = g_mode.h,
                            .flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_VULKAN | SDL_WINDOW_INPUT_FOCUS |
                                     SDL_WINDOW_KEYBOARD_GRABBED};
    g_window.props = SDL_CreateProperties();
    g_window_made = true;
    sceSystemServiceHideSplashScreen();  // (the shell's splash covers the title's frames until it goes)
    SDL_Event e = {.window = {.type = SDL_EVENT_WINDOW_FOCUS_GAINED, .windowID = 1}};
    push(&e);
    return &g_window;
}
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window *w) { return w ? w->props : 0; }
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *w) { return w ? w->flags : 0; }
SDL_WindowID SDL_GetWindowID(SDL_Window *w) { return w ? 1 : 0; }
SDL_Window *SDL_GetWindowFromID(SDL_WindowID id) { return id == 1 && g_window_made ? &g_window : NULL; }
SDL_Window *SDL_GetKeyboardFocus(void) { return g_window_made ? &g_window : NULL; }
bool SDL_GetWindowSize(SDL_Window *w, int *x, int *y) {
    if (x) *x = w ? w->w : 0;
    if (y) *y = w ? w->h : 0;
    return w != NULL;
}
bool SDL_GetWindowSizeInPixels(SDL_Window *w, int *x, int *y) { return SDL_GetWindowSize(w, x, y); }
float SDL_GetWindowPixelDensity(SDL_Window *w) { (void)w; return 1.0f; }
bool SDL_GetWindowPosition(SDL_Window *w, int *x, int *y) { (void)w; if (x) *x = 0; if (y) *y = 0; return true; }
bool SDL_GetWindowBordersSize(SDL_Window *w, int *t, int *l, int *b, int *r) {
    (void)w;
    if (t) *t = 0; if (l) *l = 0; if (b) *b = 0; if (r) *r = 0;
    return true;
}
// (the window is the TV: size, position, fullscreen and title stay as they are)
bool SDL_SetWindowSize(SDL_Window *w, int x, int y) { (void)w, (void)x, (void)y; return true; }
bool SDL_SetWindowPosition(SDL_Window *w, int x, int y) { (void)w, (void)x, (void)y; return true; }
bool SDL_SetWindowFullscreen(SDL_Window *w, bool on) { (void)w, (void)on; return true; }
bool SDL_SetWindowFullscreenMode(SDL_Window *w, const SDL_DisplayMode *m) { (void)w, (void)m; return true; }
const SDL_DisplayMode *SDL_GetWindowFullscreenMode(SDL_Window *w) { (void)w; return &g_mode; }
bool SDL_SetWindowTitle(SDL_Window *w, const char *t) { (void)w, (void)t; return true; }
bool SDL_SetWindowIcon(SDL_Window *w, SDL_Surface *s) { (void)w, (void)s; return true; }
bool SDL_RestoreWindow(SDL_Window *w) { (void)w; return true; }
bool SDL_SyncWindow(SDL_Window *w) { (void)w; return true; }
bool SDL_SetWindowMouseRect(SDL_Window *w, const SDL_Rect *r) {
    if (w) w->mouse_rect = r ? *r : (SDL_Rect){0};
    return true;
}
const SDL_Rect *SDL_GetWindowMouseRect(SDL_Window *w) { return w && w->mouse_rect.w ? &w->mouse_rect : NULL; }

SDL_Surface *SDL_CreateSurfaceFrom(int width, int height, SDL_PixelFormat format, void *pixels, int pitch) {
    SDL_Surface *s = calloc(1, sizeof *s);  // (only ever handed to SDL_SetWindowIcon here)
    if (s) s->w = width, s->h = height, s->format = format, s->pixels = pixels, s->pitch = pitch;
    return s;
}
void SDL_DestroySurface(SDL_Surface *s) { free(s); }

// Vulkan: the TV through VK_KHR_display (RADV's display on the console)
char const *const *SDL_Vulkan_GetInstanceExtensions(Uint32 *count) {
    static const char *const ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    if (count) *count = 2;
    return ext;
}

// The output the runtime asks for before its display surface (runtime/nvn/vulkan/device.c: 2160 - the TV's own
// resolution, 4K where it has it, the nearest below otherwise - with 120 Hz first when frame generation is on)
static int g_want_w, g_want_h;
static bool g_want_hi;  // a 120 Hz mode before a bigger one (frame generation on: its 60 shown as 120)
void ps5_sdl_want_output(int w, int h) { g_want_w = w, g_want_h = h; }
void ps5_sdl_want_refresh(bool high) { g_want_hi = high; }
// (Video > Resolution chosen: its size before 120 Hz - mode_better)
static bool g_size_first;
void ps5_sdl_want_size_first(bool first) { g_size_first = first; }

// Which display mode is better (SDL_Vulkan_CreateSurface): by size - nearest the window's when asked for one
// (WSWORD_PS5_OUTPUT), else the largest up to 4K - then by the higher refresh rate.
static long mode_cost(const VkDisplayModeParametersKHR *p, bool sized, int w, int h) {
    long x = p->visibleRegion.width, y = p->visibleRegion.height;
    if (sized) return labs(x - w) + labs(y - h);
    return x > 3840 || y > 2160 ? 1L << 40 : -(x * y);
}
static bool mode_better(const VkDisplayModeParametersKHR *a, const VkDisplayModeParametersKHR *b, bool sized, int w,
                        int h) {
    // (frame generation on: 120 Hz first - a 4K TV that takes 120 Hz at 1440p only gets 1440p at 120, not 4K at 60)
    bool ha = a->refreshRate >= 100000 && a->refreshRate <= 121000, hb = b->refreshRate >= 100000 && b->refreshRate <= 121000;
    long ca = mode_cost(a, sized, w, h), cb = mode_cost(b, sized, w, h);
    if (g_size_first && sized && ca != cb) return ca < cb;
    if (g_want_hi && ha != hb) return ha;
    if (ca != cb) return ca < cb;
    // the higher refresh rate (119.88 Hz where the TV takes it: frame generation's 60 shown as 120; the game's own
    // frame rates are whole fractions of it), up to 120
    long ra = a->refreshRate <= 121000 ? (long)a->refreshRate : 0, rb = b->refreshRate <= 121000 ? (long)b->refreshRate : 0;
    return ra > rb;
}

// (0.3.23) Whether the console sends HDR to the TV (Settings > Screen and Video > HDR: "On when supported" on an HDR
// TV) - VideoOut's output status, read on a handle of its own before RADV opens the output: dynamic range 2 HDR, 1
// SDR. Video > HDR's Automatic follows it (runtime/nvn/vulkan/device.c hdr_wanted). -1: not known.
int sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *param);
int sceVideoOutClose(int32_t handle);
int sceVideoOutGetOutputStatus(int32_t handle, void *status);
static int g_tv_hdr = -1;
static void tv_hdr_probe(void) {
    static bool done;
    if (done) return;
    done = true;
    int h = sceVideoOutOpen(0xff, 0, 0, NULL);
    if (h < 0) { fprintf(stderr, "[ps5] HDR: VideoOut not opened for its status (%#x)\n", (unsigned)h); return; }
    struct { uint32_t resolution, dynamic_range; uint64_t refresh, flags, reserved[5]; } st;
    memset(&st, 0, sizeof st);
    int r = sceVideoOutGetOutputStatus(h, &st);
    sceVideoOutClose(h);
    if (r == 0 && (st.dynamic_range == 1 || st.dynamic_range == 2)) g_tv_hdr = st.dynamic_range == 2;
    fprintf(stderr, "[ps5] HDR: the console's output is %s (status %#x, dynamic range %u)\n",
            g_tv_hdr < 0 ? "not known" : g_tv_hdr ? "HDR" : "SDR", (unsigned)r, st.dynamic_range);
}
int ps5_sdl_tv_hdr(void) { return g_tv_hdr; }

bool SDL_Vulkan_CreateSurface(SDL_Window *w, VkInstance inst, const struct VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
    tv_hdr_probe();
    uint32_t np = 0;
    VkPhysicalDevice phys[4];
    np = 4;
    if (vkEnumeratePhysicalDevices(inst, &np, phys) < 0 || !np) return fail("no GPU");
    uint32_t nd = 0;
    vkGetPhysicalDeviceDisplayPropertiesKHR(phys[0], &nd, NULL);
    if (!nd) return fail("no display (VK_KHR_display)");
    VkDisplayPropertiesKHR dp[4];
    nd = nd > 4 ? 4 : nd;
    vkGetPhysicalDeviceDisplayPropertiesKHR(phys[0], &nd, dp);
    VkDisplayKHR display = dp[0].display;
    uint32_t nm = 0;
    vkGetDisplayModePropertiesKHR(phys[0], display, &nm, NULL);
    VkDisplayModePropertiesKHR *mp = calloc(nm ? nm : 1, sizeof *mp);
    if (!mp || !nm) { free(mp); return fail("the display has no modes"); }
    vkGetDisplayModePropertiesKHR(phys[0], display, &nm, mp);
    // The size the runtime asks for (its mode's: 1080p or 1440p) or WSWORD_PS5_OUTPUT's, else the TV's largest up to 4K,
    // at 119.88 Hz where the TV takes it (the driver offers it once the console's 120 Hz output is on).
    // (The modes render below it and AMD FSR scales up: settings.c mode_into.)
    // (WSWORD_PS5_OUTPUT, e.g. 3840x2160, over the runtime's ask; neither: the largest up to 4K)
    const char *want = getenv("WSWORD_PS5_OUTPUT");
    bool sized = want || g_want_h > 0;
    int ww = w->w, wh = w->h;
    if (!want && g_want_h > 0) ww = g_want_w, wh = g_want_h;
    uint32_t best = 0;
    for (uint32_t i = 0; i < nm; i++) {
        fprintf(stderr, "[ps5] display mode %u: %ux%u at %.2f Hz\n", i, mp[i].parameters.visibleRegion.width,
                mp[i].parameters.visibleRegion.height, mp[i].parameters.refreshRate / 1000.0);
        if (i && mode_better(&mp[i].parameters, &mp[best].parameters, sized, ww, wh)) best = i;
    }
    VkDisplayModePropertiesKHR mode = mp[best];
    fprintf(stderr, "[ps5] display %s: %u modes, %ux%u at %.2f Hz\n", dp[0].displayName ? dp[0].displayName : "?", nm,
            mode.parameters.visibleRegion.width, mode.parameters.visibleRegion.height, mode.parameters.refreshRate / 1000.0);
    free(mp);
    uint32_t nplane = 0;
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR(phys[0], &nplane, NULL);
    uint32_t plane = 0;
    for (uint32_t i = 0; i < nplane && i < 8; i++) {
        uint32_t n = 0;
        vkGetDisplayPlaneSupportedDisplaysKHR(phys[0], i, &n, NULL);
        VkDisplayKHR ds[8];
        n = n > 8 ? 8 : n;
        vkGetDisplayPlaneSupportedDisplaysKHR(phys[0], i, &n, ds);
        bool ok = false;
        for (uint32_t k = 0; k < n; k++) ok |= ds[k] == display;
        if (ok) { plane = i; break; }
    }
    VkDisplayPlaneCapabilitiesKHR cap;
    vkGetDisplayPlaneCapabilitiesKHR(phys[0], mode.displayMode, plane, &cap);
    VkDisplayPlaneAlphaFlagBitsKHR alpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    if (!(cap.supportedAlpha & alpha))
        for (int b = 0; b < 4; b++)
            if (cap.supportedAlpha & (1u << b)) { alpha = (VkDisplayPlaneAlphaFlagBitsKHR)(1u << b); break; }
    VkDisplaySurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
                                        .displayMode = mode.displayMode, .planeIndex = plane,
                                        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .globalAlpha = 1.0f,
                                        .alphaMode = alpha, .imageExtent = mode.parameters.visibleRegion};
    VkResult r = vkCreateDisplayPlaneSurfaceKHR(inst, &si, alloc, out);
    if (r != VK_SUCCESS) return fail("vkCreateDisplayPlaneSurfaceKHR: %d", r);
    int nw = (int)mode.parameters.visibleRegion.width, nh = (int)mode.parameters.visibleRegion.height;
    g_mode.w = nw, g_mode.h = nh;
    g_mode.refresh_rate = mode.parameters.refreshRate / 1000.0f;
    if (w->w != nw || w->h != nh) {
        w->w = nw, w->h = nh;
        SDL_Event e = {.window = {.type = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, .windowID = 1, .data1 = nw, .data2 = nh}};
        push(&e);
    }
    return true;
}

// ---- mouse, cursor, keyboard, text: none on the console (a USB keyboard is for later)
SDL_MouseButtonFlags SDL_GetMouseState(float *x, float *y) { if (x) *x = 0; if (y) *y = 0; return 0; }
SDL_MouseButtonFlags SDL_GetGlobalMouseState(float *x, float *y) { return SDL_GetMouseState(x, y); }
bool SDL_SetWindowRelativeMouseMode(SDL_Window *w, bool on) { (void)w, (void)on; return true; }
bool SDL_GetWindowRelativeMouseMode(SDL_Window *w) { (void)w; return false; }
void SDL_WarpMouseInWindow(SDL_Window *w, float x, float y) { (void)w, (void)x, (void)y; }
bool SDL_CaptureMouse(bool on) { (void)on; return true; }
static bool g_cursor_shown;
bool SDL_CursorVisible(void) { return g_cursor_shown; }
bool SDL_ShowCursor(void) { g_cursor_shown = true; return true; }
bool SDL_HideCursor(void) { g_cursor_shown = false; return true; }
struct SDL_Cursor { int id; };
SDL_Cursor *SDL_CreateSystemCursor(SDL_SystemCursor id) {
    SDL_Cursor *c = malloc(sizeof *c);
    if (c) c->id = (int)id;
    return c;
}
bool SDL_SetCursor(SDL_Cursor *c) { (void)c; return true; }
void SDL_DestroyCursor(SDL_Cursor *c) { free(c); }
bool SDL_StartTextInput(SDL_Window *w) { (void)w; return true; }
bool SDL_StopTextInput(SDL_Window *w) { (void)w; return true; }
bool SDL_SetTextInputArea(SDL_Window *w, const SDL_Rect *r, int cursor) { (void)w, (void)r, (void)cursor; return true; }

// Key names, for the bindings and the menu's labels (SDL's names for the keys a binding is likely to use)
static const struct { SDL_Scancode sc; const char *name; } k_names[] = {
    {SDL_SCANCODE_RETURN, "Return"}, {SDL_SCANCODE_ESCAPE, "Escape"}, {SDL_SCANCODE_BACKSPACE, "Backspace"},
    {SDL_SCANCODE_TAB, "Tab"}, {SDL_SCANCODE_SPACE, "Space"}, {SDL_SCANCODE_MINUS, "-"}, {SDL_SCANCODE_EQUALS, "="},
    {SDL_SCANCODE_LEFTBRACKET, "["}, {SDL_SCANCODE_RIGHTBRACKET, "]"}, {SDL_SCANCODE_BACKSLASH, "\\"},
    {SDL_SCANCODE_SEMICOLON, ";"}, {SDL_SCANCODE_APOSTROPHE, "'"}, {SDL_SCANCODE_GRAVE, "`"}, {SDL_SCANCODE_COMMA, ","},
    {SDL_SCANCODE_PERIOD, "."}, {SDL_SCANCODE_SLASH, "/"}, {SDL_SCANCODE_CAPSLOCK, "CapsLock"},
    {SDL_SCANCODE_F1, "F1"}, {SDL_SCANCODE_F2, "F2"}, {SDL_SCANCODE_F3, "F3"}, {SDL_SCANCODE_F4, "F4"},
    {SDL_SCANCODE_F5, "F5"}, {SDL_SCANCODE_F6, "F6"}, {SDL_SCANCODE_F7, "F7"}, {SDL_SCANCODE_F8, "F8"},
    {SDL_SCANCODE_F9, "F9"}, {SDL_SCANCODE_F10, "F10"}, {SDL_SCANCODE_F11, "F11"}, {SDL_SCANCODE_F12, "F12"},
    {SDL_SCANCODE_INSERT, "Insert"}, {SDL_SCANCODE_HOME, "Home"}, {SDL_SCANCODE_PAGEUP, "PageUp"},
    {SDL_SCANCODE_DELETE, "Delete"}, {SDL_SCANCODE_END, "End"}, {SDL_SCANCODE_PAGEDOWN, "PageDown"},
    {SDL_SCANCODE_RIGHT, "Right"}, {SDL_SCANCODE_LEFT, "Left"}, {SDL_SCANCODE_DOWN, "Down"}, {SDL_SCANCODE_UP, "Up"},
    {SDL_SCANCODE_KP_ENTER, "Keypad Enter"}, {SDL_SCANCODE_LCTRL, "Left Ctrl"}, {SDL_SCANCODE_LSHIFT, "Left Shift"},
    {SDL_SCANCODE_LALT, "Left Alt"}, {SDL_SCANCODE_LGUI, "Left GUI"}, {SDL_SCANCODE_RCTRL, "Right Ctrl"},
    {SDL_SCANCODE_RSHIFT, "Right Shift"}, {SDL_SCANCODE_RALT, "Right Alt"}, {SDL_SCANCODE_RGUI, "Right GUI"},
};
const char *SDL_GetScancodeName(SDL_Scancode sc) {
    static _Thread_local char one[2];
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) { one[0] = (char)('A' + sc - SDL_SCANCODE_A); return one; }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) { one[0] = (char)('1' + sc - SDL_SCANCODE_1); return one; }
    if (sc == SDL_SCANCODE_0) return "0";
    for (size_t i = 0; i < sizeof k_names / sizeof *k_names; i++)
        if (k_names[i].sc == sc) return k_names[i].name;
    return "";
}
SDL_Scancode SDL_GetScancodeFromName(const char *name) {
    if (!name || !*name) return SDL_SCANCODE_UNKNOWN;
    if (!name[1]) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c >= 'A' && c <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + c - 'A');
        if (c >= '1' && c <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + c - '1');
        if (c == '0') return SDL_SCANCODE_0;
    }
    for (size_t i = 0; i < sizeof k_names / sizeof *k_names; i++)
        if (!strcasecmp(k_names[i].name, name)) return k_names[i].sc;
    return SDL_SCANCODE_UNKNOWN;
}
SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode sc, SDL_Keymod mod, bool key_event) {
    (void)mod, (void)key_event;
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return (SDL_Keycode)('a' + sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return (SDL_Keycode)('1' + sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc == SDL_SCANCODE_RETURN) return SDLK_RETURN;
    if (sc == SDL_SCANCODE_ESCAPE) return SDLK_ESCAPE;
    if (sc == SDL_SCANCODE_SPACE) return SDLK_SPACE;
    if (sc == SDL_SCANCODE_TAB) return SDLK_TAB;
    if (sc == SDL_SCANCODE_BACKSPACE) return SDLK_BACKSPACE;
    return SDL_SCANCODE_TO_KEYCODE(sc);
}
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode k, SDL_Keymod *mod) {
    if (mod) *mod = SDL_KMOD_NONE;
    if (k >= 'a' && k <= 'z') return (SDL_Scancode)(SDL_SCANCODE_A + k - 'a');
    if (k >= 'A' && k <= 'Z') { if (mod) *mod = SDL_KMOD_LSHIFT; return (SDL_Scancode)(SDL_SCANCODE_A + k - 'A'); }
    if (k >= '1' && k <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + k - '1');
    if (k == '0') return SDL_SCANCODE_0;
    if (k == SDLK_RETURN) return SDL_SCANCODE_RETURN;
    if (k == SDLK_ESCAPE) return SDL_SCANCODE_ESCAPE;
    if (k == SDLK_SPACE) return SDL_SCANCODE_SPACE;
    if (k == SDLK_TAB) return SDL_SCANCODE_TAB;
    if (k == SDLK_BACKSPACE) return SDL_SCANCODE_BACKSPACE;
    if (k & SDLK_SCANCODE_MASK) return (SDL_Scancode)(k & ~SDLK_SCANCODE_MASK);
    return SDL_SCANCODE_UNKNOWN;
}

// ---- clipboard, URLs, dialogs, message boxes, folders
static char *g_clip;
static pthread_mutex_t g_clip_lock = PTHREAD_MUTEX_INITIALIZER;
bool SDL_SetClipboardText(const char *t) {
    pthread_mutex_lock(&g_clip_lock);
    free(g_clip);
    g_clip = strdup(t ? t : "");
    pthread_mutex_unlock(&g_clip_lock);
    return true;
}
char *SDL_GetClipboardText(void) {
    pthread_mutex_lock(&g_clip_lock);
    char *t = strdup(g_clip ? g_clip : "");
    pthread_mutex_unlock(&g_clip_lock);
    return t;
}
bool SDL_OpenURL(const char *url) { (void)url; return fail("no browser for titles"); }
void SDL_ShowOpenFileDialog(SDL_DialogFileCallback cb, void *u, SDL_Window *w, const SDL_DialogFileFilter *f, int nf,
                            const char *loc, bool many) {
    (void)w, (void)f, (void)nf, (void)loc, (void)many;
    static const char *const none[] = {NULL};
    if (cb) cb(u, none, -1);  // (cancelled: there's no file picker on the console)
}
void SDL_ShowOpenFolderDialog(SDL_DialogFileCallback cb, void *u, SDL_Window *w, const char *loc, bool many) {
    SDL_ShowOpenFileDialog(cb, u, w, NULL, 0, loc, many);
}
bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char *title, const char *message, SDL_Window *w) {
    (void)flags, (void)w;
    struct { char unused[45]; char message[3075]; } req;  // (the shell's notification: what homebrew titles use)
    memset(&req, 0, sizeof req);
    snprintf(req.message, sizeof req.message, "%s: %s", title ? title : "WSword", message ? message : "");
    fprintf(stderr, "[ps5] message: %s\n", req.message);
    return sceKernelSendNotificationRequest(0, &req, sizeof req, 0) == 0;
}
bool SDL_CreateDirectory(const char *path) {
    if (!path || !*path) return fail("no path");
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(p, 0755); *c = '/'; }
    return mkdir(p, 0755) == 0 || errno == EEXIST || fail("mkdir %s: %s", path, strerror(errno));
}

// ---- the gamepad: the signed-in user's DualSense
// One sample as the console writes it (120 bytes; the PS4-compatible layout through the timestamp: orientation,
// acceleration in g, angular velocity in rad/s, then the touch data).
typedef struct {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2;
    uint8_t pad0[2];
    float orientation[4];
    float accel[3];
    float gyro[3];
    uint8_t touch[24];
    int32_t connected;
    uint64_t timestamp_us;
    uint8_t extension[16];
    uint8_t connected_count;
    uint8_t remaining[15];
} PadSample;
_Static_assert(sizeof(PadSample) == 120, "the console's pad samples are 120 bytes");
_Static_assert(__builtin_offsetof(PadSample, accel) == 0x1c, "acceleration at 0x1c");
_Static_assert(__builtin_offsetof(PadSample, connected) == 0x4c, "connection state at 0x4c");

enum {
    PS_L3 = 0x2, PS_R3 = 0x4, PS_OPTIONS = 0x8, PS_UP = 0x10, PS_RIGHT = 0x20, PS_DOWN = 0x40, PS_LEFT = 0x80,
    PS_L1 = 0x400, PS_R1 = 0x800, PS_TRIANGLE = 0x1000, PS_CIRCLE = 0x2000, PS_CROSS = 0x4000, PS_SQUARE = 0x8000,
    PS_TOUCH_PAD = 0x100000, PS_INTERCEPTED = 0x80000000u,
};
static const struct { uint32_t ps; SDL_GamepadButton sdl; } k_buttons[] = {
    {PS_CROSS, SDL_GAMEPAD_BUTTON_SOUTH}, {PS_CIRCLE, SDL_GAMEPAD_BUTTON_EAST}, {PS_SQUARE, SDL_GAMEPAD_BUTTON_WEST},
    {PS_TRIANGLE, SDL_GAMEPAD_BUTTON_NORTH}, {PS_TOUCH_PAD, SDL_GAMEPAD_BUTTON_BACK},
    {PS_OPTIONS, SDL_GAMEPAD_BUTTON_START}, {PS_L3, SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {PS_R3, SDL_GAMEPAD_BUTTON_RIGHT_STICK}, {PS_L1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {PS_R1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {PS_UP, SDL_GAMEPAD_BUTTON_DPAD_UP},
    {PS_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {PS_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {PS_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};

struct SDL_Gamepad { int unused; };
static SDL_Gamepad g_pad_obj;
#define PAD_ID 1
static pthread_mutex_t g_pad_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_pad_service, g_pad_added, g_pad_motion, g_pad_open_refs;
static int32_t g_pad_user = -1, g_pad_handle = -1;
static uint64_t g_pad_next_try, g_pad_read_ns, g_rumble_end;
static bool g_rumbling;
static float g_last_accel[3], g_last_gyro[3];  // (TSword: SDL_GetGamepadSensorData's, the latest readings)
static uint32_t g_held;          // SDL buttons down, a bit each
static Sint16 g_axes[SDL_GAMEPAD_AXIS_COUNT];
static bool g_motion_logged;

static void pad_start(void) {
    pthread_mutex_lock(&g_pad_lock);
    if (!g_pad_service) {
        sceUserServiceInitialize(NULL);
        int r = scePadInit();
        g_pad_service = true;
        fprintf(stderr, "[ps5] pad service: %d\n", r);
    }
    pthread_mutex_unlock(&g_pad_lock);
}

// ---- the DualSense's own: haptics, the light bar, the adaptive triggers, the touch pad's swipes
// (runtime/hle/dualsense.c drives them through the ps5_pad_* calls below)

// Haptics: the controller's two actuators driven as sound - its vibration audio port (sceAudioOut type 10, 48 kHz
// S16 stereo, as PS5_RetroArch's input_ps5.cpp) in the pad's advanced vibration mode. The game's HD rumble (each
// Joy-Con's low and high band: amplitude and frequency) is synthesised as it is: the left Joy-Con's on the left
// actuator, the right's on the right. Without the port, the two motors as before (compatible mode).
#define HAPTIC_RATE 48000
#define HAPTIC_GRAIN 256
static int32_t g_hap_port = -1;
static pthread_t g_hap_thread;
static bool g_hap_on;
static float g_hap_target[8];  // left: low amp, low Hz, high amp, high Hz; right: the same (under g_pad_lock)
static uint64_t g_hap_until;   // (the values stop counting: the game sends them each frame)

static void *haptics_main(void *arg) {
    (void)arg;
    int16_t out[HAPTIC_GRAIN * 2];
    double ph[4] = {0};
    float cur[8] = {0, 160, 0, 320, 0, 160, 0, 320};
    for (;;) {
        float tgt[8];
        pthread_mutex_lock(&g_pad_lock);
        memcpy(tgt, g_hap_target, sizeof tgt);
        bool live = ticks_ns() < g_hap_until;
        int32_t port = g_hap_port;
        pthread_mutex_unlock(&g_pad_lock);
        if (port < 0) break;
        if (!live) tgt[0] = tgt[2] = tgt[4] = tgt[6] = 0;
        for (int i = 0; i < HAPTIC_GRAIN; i++) {
            float t = (float)i / HAPTIC_GRAIN;
            for (int c = 0; c < 2; c++) {
                float *v = cur + 4 * c, *w = tgt + 4 * c, smp = 0;
                for (int b = 0; b < 2; b++) {
                    float amp = v[2 * b] + (w[2 * b] - v[2 * b]) * t;  // (eased across the grain: no clicks)
                    float hz = w[2 * b + 1] > 20 && w[2 * b + 1] < 1300 ? w[2 * b + 1] : (b ? 320 : 160);
                    ph[2 * c + b] += hz / HAPTIC_RATE;
                    if (ph[2 * c + b] >= 1) ph[2 * c + b] -= 1;
                    smp += amp * (float)sin(6.283185307 * ph[2 * c + b]);
                }
                smp *= 0.9f;
                smp = smp > 1 ? 1 : smp < -1 ? -1 : smp;
                out[2 * i + c] = (int16_t)(smp * 32000);
            }
        }
        memcpy(cur, tgt, sizeof cur);
        for (int b = 1; b < 8; b += 2) cur[b] = tgt[b];
        if (sceAudioOutOutput(port, out) < 0) sleep_ns(5000000);
    }
    return NULL;
}

static void haptics_open_locked(int32_t h) {
    if (g_hap_port < 0) {
        sceAudioOutInit();
        int port = sceAudioOutOpen(g_pad_user, 10, 0, HAPTIC_GRAIN, HAPTIC_RATE, 1);
        if (port >= 0 && scePadSetVibrationMode(h, 1) >= 0) {
            g_hap_port = port;
            if (pthread_create(&g_hap_thread, NULL, haptics_main, NULL) != 0) {
                sceAudioOutClose(port);
                g_hap_port = -1;
            }
        } else if (port >= 0) sceAudioOutClose(port);
        fprintf(stderr, "[ps5] pad: %s\n", g_hap_port >= 0 ? "haptics (the vibration audio port)" : "two-motor rumble");
    } else scePadSetVibrationMode(h, 1);
    g_hap_on = g_hap_port >= 0;
    if (!g_hap_on) scePadSetVibrationMode(h, 2);  // (the two-motor rumble)
}

// The game's HD rumble now (runtime/hle/vibration.c); false: no haptics, the motors (SDL_RumbleGamepad) instead
bool ps5_pad_haptics(const float v[8]) {
    pthread_mutex_lock(&g_pad_lock);
    bool on = g_hap_on;
    if (on) memcpy(g_hap_target, v, sizeof g_hap_target), g_hap_until = ticks_ns() + 200000000u;
    pthread_mutex_unlock(&g_pad_lock);
    return on;
}

// The light bar and the triggers: what the runtime wants, sent when it changes (and to a new handle)
static int32_t g_light_want = 0x0040ff, g_light_sent = -1, g_trig_want, g_trig_sent = -1;
void ps5_pad_light(uint8_t r, uint8_t g, uint8_t b) {
    pthread_mutex_lock(&g_pad_lock);
    g_light_want = r << 16 | g << 8 | b;
    pthread_mutex_unlock(&g_pad_lock);
}
// 0: the triggers free; 1: play (R2 a bow's draw - resistance to a click near the end; L2 a light, even resistance)
void ps5_pad_triggers(int mode) {
    pthread_mutex_lock(&g_pad_lock);
    g_trig_want = mode;
    pthread_mutex_unlock(&g_pad_lock);
}
// ScePadTriggerEffectParam: a mask (L2 1, R2 2), then a command for each (mode, 4 padding, 48 bytes of its parameters)
typedef struct { uint32_t mode; uint8_t pad[4]; uint8_t data[48]; } TriggerCommand;
typedef struct { uint8_t mask; uint8_t pad[7]; TriggerCommand cmd[2]; } TriggerParam;
_Static_assert(sizeof(TriggerParam) == 120, "the trigger effect parameter is 120 bytes");
static void dualsense_apply_locked(uint64_t now) {
    static uint64_t next;
    if (g_light_want != g_light_sent && now >= next) {  // (at most every 30 ms: a pulse is smooth enough)
        uint8_t c[4] = {(uint8_t)(g_light_want >> 16), (uint8_t)(g_light_want >> 8), (uint8_t)g_light_want, 0};
        if (scePadSetLightBar(g_pad_handle, c) >= 0) g_light_sent = g_light_want;
        next = now + 30000000u;
    }
    if (g_trig_want != g_trig_sent) {
        TriggerParam p = {.mask = 3};
        if (g_trig_want == 1) {
            p.cmd[0].mode = 1;                                 // L2: feedback - from position 2, strength 2 (of 8)
            p.cmd[0].data[0] = 2, p.cmd[0].data[1] = 2;
            p.cmd[1].mode = 2;                                 // R2: weapon - resistance from 2 to 7, strength 6
            p.cmd[1].data[0] = 2, p.cmd[1].data[1] = 7, p.cmd[1].data[2] = 6;
        }
        int r = scePadSetTriggerEffect(g_pad_handle, &p);
        if (g_trig_sent == -1 || r < 0) fprintf(stderr, "[ps5] pad: adaptive triggers %s: %d\n", g_trig_want ? "on" : "off", r);
        g_trig_sent = g_trig_want;
    }
}

// The touch pad: a swipe (a quick stroke across it, a finger's down to up) while the runtime says so (the pause menu)
// presses L1 or R1 for 60 ms - the menu's page to the left or the right
static bool g_swipe_on;
static uint32_t g_swipe_held;
static uint64_t g_swipe_until;
void ps5_pad_swipes(bool on) {
    pthread_mutex_lock(&g_pad_lock);
    g_swipe_on = on;
    pthread_mutex_unlock(&g_pad_lock);
}
// (TSword PS5) A two-finger tap - two fingers rested on the touch pad without clicking it and lifted within half a
// second, hardly moved - opens or closes TSword's menu (ps5/ut/ui/tsword_ui.cpp), as WSword PS5's
void tsword_ui_tap(void);
void tsword_ui_pad(uint32_t held, int16_t lx, int16_t ly);
int tsword_ui_captures(void);
static void touch_sample(const uint8_t t[24], uint64_t ts_us, bool clicked) {
    static bool down;
    static int x0, y0, x1, y1, most;
    static int f2x0, f2y0, f2x1, f2y1;  // (the second finger, from when it came)
    static bool second, pressed;
    static uint64_t t0;
    int n = t[0];
    int x = t[8] | t[9] << 8, y = t[10] | t[11] << 8;
    int x2 = t[16] | t[17] << 8, y2 = t[18] | t[19] << 8;
    if (n > 0 && !down) {
        down = true, x0 = x1 = x, y0 = y1 = y, t0 = ts_us, most = n, pressed = clicked, second = false;
    } else if (n > 0) {
        x1 = x, y1 = y;
    }
    if (n > 0) {
        if (n > most) most = n;
        pressed |= clicked;
        if (n >= 2 && !second) second = true, f2x0 = f2x1 = x2, f2y0 = f2y1 = y2;
        else if (n >= 2) f2x1 = x2, f2y1 = y2;
    } else if (down) {
        down = false;
        int dx = x1 - x0, dy = y1 - y0;
        if (g_swipe_on && most == 1 && !tsword_ui_captures() && ts_us - t0 < 700000 && abs(dx) > 450 && abs(dx) > 2 * abs(dy)) {
            g_swipe_held = 1u << (dx > 0 ? SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER : SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
            g_swipe_until = ticks_ns() + 60000000u;
        }
        int moved = abs(dx) + abs(dy), moved2 = second ? abs(f2x1 - f2x0) + abs(f2y1 - f2y0) : 0;
        if (most == 2 && second && !pressed && ts_us - t0 < 500000 && moved < 160 && moved2 < 160) {
            fprintf(stderr, "[ps5] touch pad: two-finger tap (the TSword menu)\n");
            tsword_ui_tap();
        }
    }
}

static Sint16 stick(uint8_t v) { return (Sint16)(((int)v * 2 - 255) * 32767 / 255); }
static Sint16 trigger(uint8_t v) { return (Sint16)((int)v * 32767 / 255); }

static void pad_open_locked(uint64_t now) {
    if (g_pad_handle >= 0 || now < g_pad_next_try) return;
    g_pad_next_try = now + 1000000000u;  // (again in a second: the controller may not be on yet)
    if (g_pad_user < 0 && sceUserServiceGetInitialUser(&g_pad_user) < 0) { g_pad_user = -1; return; }
    int h = scePadOpen(g_pad_user, 0, 0, NULL);
    if (h < 0) return;
    g_pad_handle = h;
    haptics_open_locked(h);
    g_light_sent = g_trig_sent = -1;  // (the light bar and the triggers: set again on the new handle)
    if (g_pad_motion) scePadSetMotionSensorState(h, true);
    fprintf(stderr, "[ps5] pad: user %d's controller open (handle %d)\n", (int)g_pad_user, h);
    if (!g_pad_added) {
        g_pad_added = true;
        SDL_Event e = {.gdevice = {.type = SDL_EVENT_GAMEPAD_ADDED, .which = PAD_ID}};
        push(&e);
    }
}

// Every sample since the last read: the state SDL keeps, and its events (buttons, axes, motion).
static void pad_read_locked(uint64_t now) {
    pad_open_locked(now);
    if (g_pad_handle < 0) return;
    if (g_rumbling && now >= g_rumble_end) {
        uint8_t off[2] = {0, 0};
        scePadSetVibration(g_pad_handle, off);
        g_rumbling = false;
    }
    if (g_swipe_held && now >= g_swipe_until) g_swipe_held = 0;  // (the swipe's tab press: let go)
    dualsense_apply_locked(now);
    PadSample s[64];
    int n = scePadRead(g_pad_handle, s, 64);
    g_pad_read_ns = now;
    for (int i = 0; i < n; i++) {
        bool ours = s[i].connected && !(s[i].buttons & PS_INTERCEPTED);  // (the shell has the pad: not input)
        uint32_t held = 0;
        Sint16 axes[SDL_GAMEPAD_AXIS_COUNT] = {0};
        if (ours) {
            for (size_t b = 0; b < sizeof k_buttons / sizeof *k_buttons; b++)
                if (s[i].buttons & k_buttons[b].ps) held |= 1u << k_buttons[b].sdl;
            axes[SDL_GAMEPAD_AXIS_LEFTX] = stick(s[i].lx), axes[SDL_GAMEPAD_AXIS_LEFTY] = stick(s[i].ly);
            axes[SDL_GAMEPAD_AXIS_RIGHTX] = stick(s[i].rx), axes[SDL_GAMEPAD_AXIS_RIGHTY] = stick(s[i].ry);
            axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = trigger(s[i].l2);
            axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = trigger(s[i].r2);
        }
        /* (TSword PS5) While TSword's menu is open it has the controller: the game sees it at rest. When the menu
           closes, what is still held from it stays hidden from the game until it is let go (the Cross that chose
           Resume is no press in the game). */
        static uint32_t ui_mask;
        if (ours) tsword_ui_pad(held, axes[SDL_GAMEPAD_AXIS_LEFTX], axes[SDL_GAMEPAD_AXIS_LEFTY]);
        if (tsword_ui_captures()) {
            ui_mask = held;
            held = 0;
            memset(axes, 0, sizeof axes);
        } else if (ui_mask) {
            ui_mask &= held;
            held &= ~ui_mask;
        }
        uint32_t changed = held ^ g_held;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
            if (changed >> b & 1) {
                SDL_Event e = {.gbutton = {.type = held >> b & 1 ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP,
                                           .which = PAD_ID, .button = (Uint8)b, .down = (held >> b & 1) != 0}};
                push(&e);
            }
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++)
            if (axes[a] != g_axes[a]) {
                SDL_Event e = {.gaxis = {.type = SDL_EVENT_GAMEPAD_AXIS_MOTION, .which = PAD_ID, .axis = (Uint8)a,
                                         .value = axes[a]}};
                push(&e);
            }
        if (ours) touch_sample(s[i].touch, s[i].timestamp_us, (s[i].buttons & PS_TOUCH_PAD) != 0);
        held |= g_swipe_held;
        g_held = held;
        memcpy(g_axes, axes, sizeof axes);
        if (g_pad_motion && ours) {
            // SDL's axes, in its units: m/s^2 and rad/s (the console's acceleration is in g)
            uint64_t ts = s[i].timestamp_us * 1000u;
            SDL_Event a = {.gsensor = {.type = SDL_EVENT_GAMEPAD_SENSOR_UPDATE, .which = PAD_ID, .sensor = SDL_SENSOR_ACCEL,
                                       .data = {s[i].accel[0] * SDL_STANDARD_GRAVITY, s[i].accel[1] * SDL_STANDARD_GRAVITY,
                                                s[i].accel[2] * SDL_STANDARD_GRAVITY},
                                       .sensor_timestamp = ts}};
            SDL_Event g = {.gsensor = {.type = SDL_EVENT_GAMEPAD_SENSOR_UPDATE, .which = PAD_ID, .sensor = SDL_SENSOR_GYRO,
                                       .data = {s[i].gyro[0], s[i].gyro[1], s[i].gyro[2]}, .sensor_timestamp = ts}};
            memcpy(g_last_accel, a.gsensor.data, sizeof g_last_accel);
            memcpy(g_last_gyro, g.gsensor.data, sizeof g_last_gyro);
            push(&g);
            push(&a);
            if (!g_motion_logged) {  // (the first reading in the log: the axes are checked against it)
                g_motion_logged = true;
                fprintf(stderr, "[ps5] motion: accel %.2f %.2f %.2f g, gyro %.3f %.3f %.3f rad/s\n", s[i].accel[0],
                        s[i].accel[1], s[i].accel[2], s[i].gyro[0], s[i].gyro[1], s[i].gyro[2]);
            }
        }
    }
}

static void pump(void) {
    if (!g_pad_service) return;
    pthread_mutex_lock(&g_pad_lock);
    pad_read_locked(ticks_ns());
    pthread_mutex_unlock(&g_pad_lock);
}

// The state as of the last read; read again when that was more than 4 ms ago (a caller between pumps).
static void pad_fresh(void) {
    if (!g_pad_service) return;
    pthread_mutex_lock(&g_pad_lock);
    uint64_t now = ticks_ns();
    if (now - g_pad_read_ns > 4000000u) pad_read_locked(now);
    pthread_mutex_unlock(&g_pad_lock);
}

SDL_JoystickID *SDL_GetGamepads(int *count) {
    pump();
    SDL_JoystickID *ids = calloc(2, sizeof *ids);
    int n = g_pad_handle >= 0 && ids ? 1 : 0;
    if (n) ids[0] = PAD_ID;
    if (count) *count = n;
    return ids;
}
bool SDL_HasGamepad(void) { pump(); return g_pad_handle >= 0; }
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id) {
    if (id != PAD_ID || g_pad_handle < 0) { fail("no such gamepad"); return NULL; }
    g_pad_open_refs = true;
    return &g_pad_obj;
}
void SDL_CloseGamepad(SDL_Gamepad *p) { (void)p; g_pad_open_refs = false; }
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *p) { return p ? PAD_ID : 0; }
const char *SDL_GetGamepadName(SDL_Gamepad *p) { return p ? "DualSense Wireless Controller" : NULL; }
const char *SDL_GetGamepadNameForID(SDL_JoystickID id) { return id == PAD_ID ? "DualSense Wireless Controller" : NULL; }
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *p) { return p ? SDL_GAMEPAD_TYPE_PS5 : SDL_GAMEPAD_TYPE_UNKNOWN; }
SDL_GamepadType SDL_GetGamepadTypeForID(SDL_JoystickID id) { return id == PAD_ID ? SDL_GAMEPAD_TYPE_PS5 : SDL_GAMEPAD_TYPE_UNKNOWN; }
bool SDL_GetGamepadButton(SDL_Gamepad *p, SDL_GamepadButton b) {
    if (!p || b < 0 || b >= SDL_GAMEPAD_BUTTON_COUNT) return false;
    pad_fresh();
    return g_held >> b & 1;
}
Sint16 SDL_GetGamepadAxis(SDL_Gamepad *p, SDL_GamepadAxis a) {
    if (!p || a < 0 || a >= SDL_GAMEPAD_AXIS_COUNT) return 0;
    pad_fresh();
    return g_axes[a];
}
bool SDL_GamepadHasSensor(SDL_Gamepad *p, SDL_SensorType t) { return p && (t == SDL_SENSOR_ACCEL || t == SDL_SENSOR_GYRO); }
bool SDL_SetGamepadSensorEnabled(SDL_Gamepad *p, SDL_SensorType t, bool on) {
    if (!SDL_GamepadHasSensor(p, t)) return fail("no such sensor");
    pthread_mutex_lock(&g_pad_lock);
    if (on && !g_pad_motion && g_pad_handle >= 0) scePadSetMotionSensorState(g_pad_handle, true);
    g_pad_motion = on || g_pad_motion;
    pthread_mutex_unlock(&g_pad_lock);
    return true;
}
float SDL_GetGamepadSensorDataRate(SDL_Gamepad *p, SDL_SensorType t) { return SDL_GamepadHasSensor(p, t) ? 250.0f : 0; }
bool SDL_RumbleGamepad(SDL_Gamepad *p, Uint16 low, Uint16 high, Uint32 ms) {
    if (!p) return fail("no gamepad");
    pthread_mutex_lock(&g_pad_lock);
    bool ok = false;
    if (g_pad_handle >= 0) {
        uint8_t v[2] = {(uint8_t)(low >> 8), (uint8_t)(high >> 8)};  // (the large motor, the small one)
        ok = scePadSetVibration(g_pad_handle, v) >= 0;
        g_rumbling = (low | high) != 0;
        g_rumble_end = ticks_ns() + (uint64_t)(ms ? ms : 0xFFFFFFFFu) * 1000000u;
    }
    pthread_mutex_unlock(&g_pad_lock);
    return ok || fail("rumble refused");
}

// ---- audio: every stream mixed into the console's one 48 kHz stereo output
#define OUT_RATE 48000
#define GRAIN 256
struct SDL_AudioStream {
    SDL_AudioSpec spec;
    float gain;
    SDL_AudioStreamCallback get_cb;
    void *get_user;
    uint8_t *buf;          // queued input bytes: [head, head + len) in a ring of cap
    size_t cap, head, len;
    double pos;            // resampling: the read position within the next input frame
    float prev[8];         // the last frame consumed (for the linear interpolation)
    bool paused;
    SDL_AudioStream *next;
};
static SDL_AudioStream *g_streams;
static pthread_mutex_t g_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_audio_port = -1;
static pthread_t g_audio_thread;

static int frame_bytes(const SDL_AudioSpec *s) { return SDL_AUDIO_BYTESIZE(s->format) * s->channels; }

static float sample_at(const SDL_AudioStream *st, size_t byte) {
    const uint8_t *p = st->buf + (st->head + byte) % st->cap;
    switch (st->spec.format) {
    case SDL_AUDIO_F32: { float f; memcpy(&f, p, 4); return f; }
    case SDL_AUDIO_S16: { int16_t v; memcpy(&v, p, 2); return v / 32768.0f; }
    case SDL_AUDIO_S32: { int32_t v; memcpy(&v, p, 4); return v / 2147483648.0f; }
    case SDL_AUDIO_U8: return (*p - 128) / 128.0f;
    case SDL_AUDIO_S8: return (int8_t)*p / 128.0f;
    default: return 0;
    }
}

// One input frame as stereo (5.1: SDL's channel order, folded down with the usual weights).
static void frame_stereo(const SDL_AudioStream *st, size_t off, float out[2]) {
    int c = st->spec.channels, b = SDL_AUDIO_BYTESIZE(st->spec.format);
    float s[8] = {0};
    for (int i = 0; i < c && i < 8; i++) s[i] = sample_at(st, off + (size_t)i * b);
    if (c == 1) out[0] = out[1] = s[0];
    else if (c >= 6) {
        out[0] = s[0] + 0.707f * s[2] + 0.707f * s[4];
        out[1] = s[1] + 0.707f * s[2] + 0.707f * s[5];
    } else out[0] = s[0], out[1] = s[1];
}

// Mix every stream's next GRAIN frames at 48 kHz into acc (stereo floats). With the lock.
static void mix_locked(float *acc) {
    for (SDL_AudioStream *st = g_streams; st; st = st->next) {
        if (st->paused) continue;
        int fb = frame_bytes(&st->spec);
        double step = (double)st->spec.freq / OUT_RATE;
        size_t need = (size_t)ceil(GRAIN * step + 2) * fb;
        if (st->get_cb && st->len < need) {  // (the callback queues more, as SDL's get callback does)
            SDL_AudioStreamCallback cb = st->get_cb;
            pthread_mutex_unlock(&g_audio_lock);
            cb(st->get_user, st, (int)(need - st->len), (int)need);
            pthread_mutex_lock(&g_audio_lock);
        }
        for (int i = 0; i < GRAIN; i++) {
            if (st->len < (size_t)fb) break;  // (underrun: silence for the rest)
            float cur[2];
            frame_stereo(st, 0, cur);
            float l = st->prev[0] + (cur[0] - st->prev[0]) * (float)st->pos;
            float r = st->prev[1] + (cur[1] - st->prev[1]) * (float)st->pos;
            acc[i * 2] += l * st->gain, acc[i * 2 + 1] += r * st->gain;
            st->pos += step;
            while (st->pos >= 1.0 && st->len >= (size_t)fb) {  // (input frames passed: consumed)
                frame_stereo(st, 0, st->prev);
                st->head = (st->head + fb) % st->cap, st->len -= fb;
                st->pos -= 1.0;
            }
        }
    }
}

static void *audio_main(void *unused) {
    (void)unused;
    static float acc[GRAIN * 2];
    static int16_t out[GRAIN * 2] __attribute__((aligned(64)));
    for (;;) {
        memset(acc, 0, sizeof acc);
        pthread_mutex_lock(&g_audio_lock);
        mix_locked(acc);
        pthread_mutex_unlock(&g_audio_lock);
        for (int i = 0; i < GRAIN * 2; i++) {
            float v = acc[i] * 32767.0f;
            out[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : lrintf(v));
        }
        if (sceAudioOutOutput(g_audio_port, out) < 0) sleep_ns(5000000);  // (blocks for one grain: the pace)
    }
    return NULL;
}

static bool audio_start_device(void) {
    pthread_mutex_lock(&g_audio_lock);
    bool ok = g_audio_port >= 0;
    if (!ok) {
        int r = sceAudioOutInit();
        (void)r;  // (already initialised is fine)
        int port = sceAudioOutOpen(0xff, 0, 0, GRAIN, OUT_RATE, 1);  // the system user's main port, S16 stereo
        if (port >= 0 && pthread_create(&g_audio_thread, NULL, audio_main, NULL) == 0) {
            g_audio_port = port;
            ok = true;
            fprintf(stderr, "[ps5] audio: 48 kHz stereo on port %d\n", port);
        } else fail("sceAudioOutOpen: %x", (unsigned)port);
    }
    pthread_mutex_unlock(&g_audio_lock);
    return ok;
}

bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID dev, SDL_AudioSpec *spec, int *frames) {
    (void)dev;
    if (spec) *spec = (SDL_AudioSpec){SDL_AUDIO_F32, 2, OUT_RATE};
    if (frames) *frames = GRAIN;
    return true;
}
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID dev, const SDL_AudioSpec *spec, SDL_AudioStreamCallback cb,
                                           void *user) {
    (void)dev;
    if (!spec || spec->channels < 1 || spec->channels > 8 || spec->freq <= 0) { fail("bad audio spec"); return NULL; }
    if (!audio_start_device()) return NULL;
    SDL_AudioStream *st = calloc(1, sizeof *st);
    if (!st) return NULL;
    st->spec = *spec;
    st->gain = 1.0f;
    st->get_cb = cb, st->get_user = user;
    st->cap = (size_t)frame_bytes(spec) * (size_t)spec->freq;  // (a second of input)
    st->buf = malloc(st->cap);
    st->paused = true;  // (as SDL: until SDL_ResumeAudioStreamDevice)
    if (!st->buf) { free(st); return NULL; }
    pthread_mutex_lock(&g_audio_lock);
    st->next = g_streams, g_streams = st;
    pthread_mutex_unlock(&g_audio_lock);
    return st;
}
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *st) { return st ? SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK : 0; }
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *st) {
    if (!st) return false;
    pthread_mutex_lock(&g_audio_lock);
    st->paused = false;
    pthread_mutex_unlock(&g_audio_lock);
    return true;
}
bool SDL_SetAudioStreamGetCallback(SDL_AudioStream *st, SDL_AudioStreamCallback cb, void *user) {
    if (!st) return false;
    pthread_mutex_lock(&g_audio_lock);
    st->get_cb = cb, st->get_user = user;
    pthread_mutex_unlock(&g_audio_lock);
    return true;
}
bool SDL_PutAudioStreamData(SDL_AudioStream *st, const void *data, int len) {
    if (!st || len < 0) return false;
    pthread_mutex_lock(&g_audio_lock);
    if (st->len + (size_t)len > st->cap) {  // (more than a second queued: the ring grows, as SDL's queue does)
        size_t cap = st->cap;
        while (st->len + (size_t)len > cap) cap *= 2;
        uint8_t *b = malloc(cap);
        if (!b) { pthread_mutex_unlock(&g_audio_lock); return false; }
        for (size_t i = 0; i < st->len; i++) b[i] = st->buf[(st->head + i) % st->cap];
        free(st->buf);
        st->buf = b, st->cap = cap, st->head = 0;
    }
    const uint8_t *s = data;
    size_t tail = (st->head + st->len) % st->cap, first = st->cap - tail < (size_t)len ? st->cap - tail : (size_t)len;
    memcpy(st->buf + tail, s, first);
    memcpy(st->buf, s + first, (size_t)len - first);
    st->len += (size_t)len;
    pthread_mutex_unlock(&g_audio_lock);
    return true;
}
int SDL_GetAudioStreamQueued(SDL_AudioStream *st) {
    if (!st) return -1;
    pthread_mutex_lock(&g_audio_lock);
    int n = (int)st->len;
    pthread_mutex_unlock(&g_audio_lock);
    return n;
}
float SDL_GetAudioStreamGain(SDL_AudioStream *st) { return st ? st->gain : -1; }
bool SDL_SetAudioStreamGain(SDL_AudioStream *st, float g) {
    if (!st || g < 0) return false;
    st->gain = g;
    return true;
}
void SDL_DestroyAudioStream(SDL_AudioStream *st) {
    if (!st) return;
    pthread_mutex_lock(&g_audio_lock);
    for (SDL_AudioStream **p = &g_streams; *p; p = &(*p)->next)
        if (*p == st) { *p = st->next; break; }
    pthread_mutex_unlock(&g_audio_lock);
    free(st->buf);
    free(st);
}

// ---- (TSword PS5) What TotK Recomp's runtime calls besides WSword's (ps5/tsword/missing.py): its window made with
// SDL_CreateWindow, the gamepad's state polled (SDL_UpdateGamepads, the sensors' latest readings), the trigger rumble,
// the version, and the virtual joystick its input layer can make (none on the console).
SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags) {
    (void)title, (void)w, (void)h, (void)flags;
    return SDL_CreateWindowWithProperties(0);
}
float SDL_GetWindowDisplayScale(SDL_Window *w) { (void)w; return 1.0f; }
int SDL_GetVersion(void) { return SDL_VERSION; }
void SDL_QuitSubSystem(SDL_InitFlags flags) { (void)flags; }
Uint32 SDL_RegisterEvents(int n) {
    static Uint32 next = SDL_EVENT_USER;
    if (n <= 0 || next + (Uint32)n > SDL_EVENT_LAST) return 0;
    Uint32 first = next;
    next += (Uint32)n;
    return first;
}
void SDL_SetEventEnabled(Uint32 type, bool on) { (void)type, (void)on; }
void SDL_UpdateGamepads(void) { pump(); }
bool SDL_GamepadConnected(SDL_Gamepad *p) { pump(); return p && g_pad_handle >= 0; }
SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad *p) {
    static SDL_PropertiesID props;
    if (!p) return 0;
    if (!props) {
        props = SDL_CreateProperties();
        SDL_SetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, true);
    }
    return props;
}
SDL_GamepadButtonLabel SDL_GetGamepadButtonLabel(SDL_Gamepad *p, SDL_GamepadButton b) {
    if (!p) return SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    switch (b) {  // (the DualSense's own symbols)
    case SDL_GAMEPAD_BUTTON_SOUTH: return SDL_GAMEPAD_BUTTON_LABEL_CROSS;
    case SDL_GAMEPAD_BUTTON_EAST: return SDL_GAMEPAD_BUTTON_LABEL_CIRCLE;
    case SDL_GAMEPAD_BUTTON_WEST: return SDL_GAMEPAD_BUTTON_LABEL_SQUARE;
    case SDL_GAMEPAD_BUTTON_NORTH: return SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE;
    default: return SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    }
}
bool SDL_GetGamepadSensorData(SDL_Gamepad *p, SDL_SensorType t, float *data, int n) {
    if (!p || !data || n <= 0) return false;
    const float *v = t == SDL_SENSOR_ACCEL ? g_last_accel : t == SDL_SENSOR_GYRO ? g_last_gyro : NULL;
    if (!v) return false;
    for (int i = 0; i < n && i < 3; i++) data[i] = v[i];
    return true;
}
bool SDL_RumbleGamepadTriggers(SDL_Gamepad *p, Uint16 left, Uint16 right, Uint32 ms) {
    (void)p, (void)left, (void)right, (void)ms;
    return true;   // (the DualSense's triggers rumble through their adaptive effects; none asked for this way yet)
}
SDL_JoystickID SDL_AttachVirtualJoystick(const SDL_VirtualJoystickDesc *desc) {
    (void)desc;
    fail("no virtual joysticks on the console");
    return 0;
}
SDL_Joystick *SDL_OpenJoystick(SDL_JoystickID id) { (void)id; fail("no joysticks but the gamepad"); return NULL; }
bool SDL_SetJoystickVirtualAxis(SDL_Joystick *j, int axis, Sint16 value) { (void)j, (void)axis, (void)value; return false; }
const char *SDL_GetAudioDeviceName(SDL_AudioDeviceID dev) { (void)dev; return "TV"; }
bool SDL_Vulkan_LoadLibrary(const char *path) { (void)path; return true; }   // (RADV is linked into the title)
bool SDL_Vulkan_GetPresentationSupport(VkInstance inst, VkPhysicalDevice phys, Uint32 family) {
    (void)inst, (void)phys;
    return family == 0;   // (the display's surface presents from the graphics queue)
}
