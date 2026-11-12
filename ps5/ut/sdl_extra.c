// (TSword PS5) The SDL3 functions UltraTears calls beyond what WSword PS5's subset (sdl/sdl_ps5.c) has: the gamepad's
// names and ids, a keyboard that is never pressed, mutexes, the Vulkan loader's entry (RADV linked into the title) and
// stand-ins for what the console has no use for (the window icon's PNG, the Steam handle).
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

// ---- strings
int SDL_strcmp(const char *a, const char *b) { return strcmp(a, b); }
int SDL_strcasecmp(const char *a, const char *b) { return strcasecmp(a, b); }

// ---- start-up
void SDL_SetMainReady(void) {}
SDL_InitFlags SDL_WasInit(SDL_InitFlags flags) { return flags; }   // (the subset's parts are always there)

// ---- mutexes
struct SDL_Mutex { pthread_mutex_t m; };
SDL_Mutex *SDL_CreateMutex(void) {
    SDL_Mutex *m = malloc(sizeof *m);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);   // (SDL's are recursive)
    if (m) pthread_mutex_init(&m->m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}
void SDL_LockMutex(SDL_Mutex *m) { if (m) pthread_mutex_lock(&m->m); }
void SDL_UnlockMutex(SDL_Mutex *m) { if (m) pthread_mutex_unlock(&m->m); }
void SDL_DestroyMutex(SDL_Mutex *m) { if (m) pthread_mutex_destroy(&m->m), free(m); }

// ---- the keyboard: none on the console (the pad's binds drive the game)
const bool *SDL_GetKeyboardState(int *numkeys) {
    static bool none[SDL_SCANCODE_COUNT];
    if (numkeys) *numkeys = SDL_SCANCODE_COUNT;
    return none;
}
bool SDL_StartTextInputWithProperties(SDL_Window *w, SDL_PropertiesID props) { (void)props; return SDL_StartTextInput(w); }

// ---- the gamepad: SDL's names for its buttons and axes (the binds in ultratears.cfg use them), a DualSense's ids
static const char *const k_buttons[] = {"a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
                                        "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1",
                                        "paddle1", "paddle2", "paddle3", "paddle4", "touchpad"};
static const char *const k_axes[] = {"leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"};

SDL_GamepadButton SDL_GetGamepadButtonFromString(const char *s) {
    for (int i = 0; s && i < (int)(sizeof k_buttons / sizeof *k_buttons); i++)
        if (!strcasecmp(s, k_buttons[i])) return (SDL_GamepadButton)i;
    return SDL_GAMEPAD_BUTTON_INVALID;
}
SDL_GamepadAxis SDL_GetGamepadAxisFromString(const char *s) {
    for (int i = 0; s && i < (int)(sizeof k_axes / sizeof *k_axes); i++)
        if (!strcasecmp(s, k_axes[i])) return (SDL_GamepadAxis)i;
    return SDL_GAMEPAD_AXIS_INVALID;
}
const char *SDL_GetGamepadPath(SDL_Gamepad *p) { (void)p; return NULL; }
Uint16 SDL_GetGamepadVendor(SDL_Gamepad *p) { return p ? 0x054c : 0; }    // Sony
Uint16 SDL_GetGamepadProduct(SDL_Gamepad *p) { return p ? 0x0ce6 : 0; }   // DualSense
Uint64 SDL_GetGamepadSteamHandle(SDL_Gamepad *p) { (void)p; return 0; }

// ---- images: the window icon has no place on the console
SDL_IOStream *SDL_IOFromConstMem(const void *mem, size_t size) { (void)mem, (void)size; return NULL; }
SDL_Surface *SDL_LoadPNG_IO(SDL_IOStream *src, bool closeio) { (void)src, (void)closeio; return NULL; }

// ---- Vulkan: the driver's own entry point (RADV is linked into the title; it has no loader)
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
SDL_FunctionPointer SDL_Vulkan_GetVkGetInstanceProcAddr(void) { return (SDL_FunctionPointer)vk_icdGetInstanceProcAddr; }
