// The game's entry into graphics: nv::SetGraphicsAllocator / nv::SetGraphicsDevtoolsAllocator / nv::InitializeGraphics.
// Ported from MacNativeGames games/totk/runtime/gfx_bridge.c @b75be8c (MIT) without its diagnostic watchpoint.
// The allocator callbacks are guest functions: kept as guest addresses (host code calls them through GCALL).
#include "compat/gfx_compat.h"
#include "host/hle.h"

typedef struct { uint64_t alloc, free, realloc, user; } GuestAllocator;
GuestAllocator g_nv_alloc, g_nv_devtools_alloc;
extern uint64_t g_t0_ns;

static void graphics_allocator(uint64_t a, uint64_t f, uint64_t r, uint64_t u) { g_nv_alloc = (GuestAllocator){a, f, r, u}; }
static void graphics_dev_allocator(uint64_t a, uint64_t f, uint64_t r, uint64_t u) { g_nv_devtools_alloc = (GuestAllocator){a, f, r, u}; }
static void graphics_initialize(void *mem, uint64_t size) {
    if (!mem || !size) runtime_fail("invalid graphics work memory");
    g_t0_ns = runtime_now_ns();
    fprintf(stderr, "[totk] graphics initialized: %llu KiB work memory at %p (backend: none yet)\n",
            (unsigned long long)(size >> 10), mem);
}

HLE_NATIVE("_ZN2nv18InitializeGraphicsEPvm", graphics_initialize, 'v')
HLE_NATIVE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", graphics_allocator, 'v')
HLE_NATIVE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", graphics_dev_allocator, 'v')
