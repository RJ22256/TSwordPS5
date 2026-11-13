// (TSword PS5) Frame generation: Lossless Scaling's, with the player's own Lossless.dll (lsfg.cpp on
// ps5/third_party/lsfg-vk, as WSword PS5's), run by the present thread (ultratears/src/vk/present.c) on the game's
// device and queue. framegen= in ultratears.cfg (TSword's menu sets it): 2, 3 or 4 - the frames on the TV for each of
// the game's; at 30 the game's, 60, 90 or 120 on the TV (90 and 120 want a 120 Hz TV).
//
// On each new game frame: its picture (the game's output image, its crop) is copied into the backend's source image
// for this frame; that submission signals the backend's timeline semaphore; the backend's passes (submitted under the
// queue's lock: the game's replayer shares the queue) write the frames between the last two real ones; once they are
// made they are copied into images of the present thread's own kind (B8G8R8A8, TRANSFER_SRC_OPTIMAL: shown as the game's
// frames are, FSR 1 and the menu over them), presented in turn, spaced evenly, and the real frame after them. The game
// frame is shown a frame later than without: what interpolation costs.
//
// No Lossless.dll ships with TSword: the player copies theirs (from their Lossless Scaling, on Steam) to the title
// folder's lsfg/Lossless.dll; WSword PS5's copy (/data/homebrew/PPSA99777/lsfg/Lossless.dll) is found too.
// SPDX-License-Identifier: GPL-3.0-or-later (it links lsfg-vk, GPL-3.0: ps5/third_party/lsfg-vk/README.TSword.md)
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "vk.h"
#include "lsfg.h"

#include <SDL3/SDL_vulkan.h>

#define LOG(...) fprintf(stderr, "[framegen] " __VA_ARGS__)

int tsword_ui_fg_mode(void);   // tsword_ui.cpp: the setting, 0 off, 1..3 the frames made between two of the game's

typedef struct { VkImage img; uint32_t w, h; VkSemaphore timeline; uint64_t value; } tsword_fg_image;

static struct {
    int failed_for;               // the setting it stopped at (0: not stopped); err: why
    char err[400];
    int open, gen, flow;          // a context: im, for these
    LsfgImages im;
    VkCommandPool pool;
    VkCommandBuffer cb[2];        // 0 the game's picture into the source, 1 the made frames out
    VkFence fence[2];
    VkSemaphore done;             // (timeline) the made frames copied out: done_value
    uint64_t done_value;
    VkImage img[LSFG_MAX_GENERATED];
    VkDeviceMemory mem[LSFG_MAX_GENERATED];
    uint32_t w, h;                // (the images' size)
    uint64_t made, shown, t_log;  // (the log line every 10 s)
    double rate_t0;               // (the generated frames a second)
    uint64_t rate_n;
    _Atomic float gen_fps;
} G;

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

// (absolute: the backend reads it with C++'s streams, which may not go through the title's working directory -
// ps5_cwd.c - as the C library's calls here do)
static const char *dll_path(void) {
    static const char *const paths[] = {"lsfg/Lossless.dll", "Lossless.dll", "/data/homebrew/PPSA99777/lsfg/Lossless.dll"};
    static char abs[700];
    struct stat st;
    for (unsigned i = 0; i < sizeof paths / sizeof *paths; i++) {
        if (stat(paths[i], &st) != 0 || st.st_size <= 0) continue;
        char cwd[512];
        if (paths[i][0] != '/' && getcwd(cwd, sizeof cwd)) snprintf(abs, sizeof abs, "%s/%s", cwd, paths[i]);
        else snprintf(abs, sizeof abs, "%s", paths[i]);
        return abs;
    }
    return NULL;
}

// A Lossless.dll is there (the menu offers frame generation): looked again every 2 s
int tsword_fg_available(void) {
    static double next;
    static int have;
    double t = now_s();
    if (t >= next) next = t + 2, have = dll_path() != NULL;
    return have;
}

int tsword_fg_generated_fps(float *fps) {
    *fps = atomic_load(&G.gen_fps);
    return *fps > 0;
}

// (the menu's line about it) NULL: nothing to say
const char *tsword_fg_status(void) { return G.failed_for ? G.err : NULL; }

static void images_free(void) {
    for (int i = 0; i < LSFG_MAX_GENERATED; i++) {
        if (G.img[i]) vk.vkDestroyImage(vkc.device, G.img[i], NULL);
        if (G.mem[i]) vk.vkFreeMemory(vkc.device, G.mem[i], NULL);
        G.img[i] = VK_NULL_HANDLE, G.mem[i] = VK_NULL_HANDLE;
    }
    G.w = G.h = 0;
}

static void off(void) {
    if (!G.open) return;
    vk.vkWaitForFences(vkc.device, 2, G.fence, VK_TRUE, UINT64_MAX);
    vk_queue_lock();   // (closing waits for the device: the queue held meanwhile)
    lsfg_close();
    vk_queue_unlock();
    images_free();
    G.open = 0;
    atomic_store(&G.gen_fps, 0.0f);
}

static void stop(int gen, const char *why) {
    snprintf(G.err, sizeof G.err, "%s", why);
    G.failed_for = gen;
    LOG("stopped: %s\n", why);
    off();
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static int objects(void) {   // the command buffers, fences and the timeline (once)
    if (G.pool) return 1;
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                  .queueFamilyIndex = vkc.family};
    if (vk.vkCreateCommandPool(vkc.device, &pi, NULL, &G.pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = G.pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
    if (vk.vkAllocateCommandBuffers(vkc.device, &ai, G.cb) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < 2; i++) if (vk.vkCreateFence(vkc.device, &fi, NULL, &G.fence[i]) != VK_SUCCESS) return 0;
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
    return vk.vkCreateSemaphore(vkc.device, &si, NULL, &G.done) == VK_SUCCESS;
}

static int images(uint32_t w, uint32_t h, int n) {   // the made frames' images, of the present thread's kind
    if (G.w == w && G.h == h && G.img[n - 1]) return 1;
    images_free();
    for (int i = 0; i < n; i++) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        if (vk.vkCreateImage(vkc.device, &ci, NULL, &G.img[i]) != VK_SUCCESS) { G.img[i] = VK_NULL_HANDLE; return 0; }
        VkMemoryRequirements req;
        vk.vkGetImageMemoryRequirements(vkc.device, G.img[i], &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                   .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(vkc.device, &ai, NULL, &G.mem[i]) != VK_SUCCESS) {
            G.mem[i] = VK_NULL_HANDLE;
            return 0;
        }
        vk.vkBindImageMemory(vkc.device, G.img[i], G.mem[i], 0);
    }
    G.w = w, G.h = h;
    return 1;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst, .oldLayout = from,
                              .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static int submit(int which, const VkSemaphoreSubmitInfo *waits, uint32_t nwait, VkSemaphore sig, uint64_t value) {
    VkSemaphoreSubmitInfo s = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = sig, .value = value,
                               .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkCommandBufferSubmitInfo c = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = G.cb[which]};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = nwait, .pWaitSemaphoreInfos = waits,
                        .commandBufferInfoCount = 1, .pCommandBufferInfos = &c, .signalSemaphoreInfoCount = 1,
                        .pSignalSemaphoreInfos = &s};
    vk_queue_lock();
    VkResult r = vk.vkQueueSubmit2(vkc.queue, 1, &si, G.fence[which]);
    vk_queue_unlock();
    return r == VK_SUCCESS;
}

static void begin(int which) {
    vk.vkWaitForFences(vkc.device, 1, &G.fence[which], VK_TRUE, UINT64_MAX);
    vk.vkResetFences(vkc.device, 1, &G.fence[which]);
    vk.vkResetCommandBuffer(G.cb[which], 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkBeginCommandBuffer(G.cb[which], &bi);
}

// The game's new frame: src (its output image, TRANSFER_SRC_OPTIMAL once wait_sem reaches wait_value), the picture its
// crop (x, y, w, h; 0 size: the whole image). Fills out[] with the frames to show before it and returns how many (0:
// none - off, stopped, or the first frame, with none before it to make frames from).
int tsword_fg_frame(VkImage src, uint32_t src_w, uint32_t src_h, const int crop[4], VkSemaphore wait_sem, uint64_t wait_value,
                    tsword_fg_image *out) {
    int want = tsword_ui_fg_mode();
    want = want >= 1 && want <= LSFG_MAX_GENERATED ? want : 0;
    if (!want) {
        if (G.open) { off(); LOG("off\n"); }
        G.failed_for = 0;
        return 0;
    }
    if (G.failed_for == want) return 0;
    G.failed_for = 0;
    int sx = crop[0], sy = crop[1], sw = crop[2], sh = crop[3];
    if (sw <= 0 || sh <= 0 || sx + sw > (int)src_w || sy + sh > (int)src_h) sx = sy = 0, sw = (int)src_w, sh = (int)src_h;
    if (sw < 64 || sh < 64) return 0;
    char err[400];
    if (!objects()) { stop(want, "no command buffers for it"); return 0; }
    double t = now_s();
    if (t - G.rate_t0 >= 1.0) {
        atomic_store(&G.gen_fps, G.rate_t0 > 0 ? (float)(G.rate_n / (t - G.rate_t0)) : 0.0f);
        G.rate_t0 = t, G.rate_n = 0;
    }
    if (t - G.t_log >= 10) {
        if (G.t_log && G.made) LOG("%dx: %llu frames made, %llu shown\n", G.gen + 1, (unsigned long long)G.made, (unsigned long long)G.shown);
        G.t_log = t, G.made = G.shown = 0;
    }
    if (!lsfg_ready()) {
        const char *dll = dll_path();
        if (!dll) { stop(want, "no Lossless.dll: copy yours to the title folder's lsfg/Lossless.dll"); return 0; }
        VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
        vk.vkGetPhysicalDeviceFeatures2(vkc.gpu, &f);   // (vk.c enables every feature the GPU has)
        char cache[720];
        snprintf(cache, sizeof cache, "%s", dll);
        char *slash = strrchr(cache, '/');
        snprintf(slash ? slash + 1 : cache, sizeof cache - (size_t)(slash ? slash + 1 - cache : 0), "pipeline_cache.bin");
        LsfgDevice d = {.instance = vkc.instance, .phys = vkc.gpu, .dev = vkc.device, .qfam = vkc.family, .queue = vkc.queue,
                        .gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr(), .fp16 = f12.shaderFloat16 != 0,
                        .cache_path = cache};
        double t0 = now_s();
        vk_queue_lock();
        int r = lsfg_init(&d, dll, true, err, sizeof err);
        vk_queue_unlock();
        if (r) { stop(want, err); return 0; }
        LOG("started: %s's %s shaders (%.0f ms)\n", dll, lsfg_low_precision() ? "FP16" : "full-precision", (now_s() - t0) * 1e3);
    }
    // the flow scale: about 540 lines of motion (WSword PS5's: at Lossless Scaling's 1080 its passes took the GPU time
    // the game needed) - 50% at 1080p, 40% at 1440p, 25% at 4K
    int flow = (int)lround(540.0 * 100 / sh / 5) * 5;
    flow = flow < 25 ? 25 : flow > 100 ? 100 : flow;
    if (!G.open || G.gen != want || G.im.width != (uint32_t)sw || G.im.height != (uint32_t)sh || G.flow != flow) {
        off();
        double t0 = now_s();
        vk_queue_lock();
        int r = lsfg_open(want, (uint32_t)sw, (uint32_t)sh, flow / 100.0f, false, &G.im, err, sizeof err);
        vk_queue_unlock();
        if (r) { stop(want, err); return 0; }
        G.open = 1, G.gen = want, G.flow = flow;
        if (!images((uint32_t)sw, (uint32_t)sh, want)) { stop(want, "no memory for its frames"); return 0; }
        LOG("%dx at %dx%d, flow scale %d%%, the backend's images format %d (%.0f ms)\n", want + 1, sw, sh, flow, (int)G.im.format,
            (now_s() - t0) * 1e3);
    }
    uint64_t s = lsfg_next_signal(), index = lsfg_frame_index();
    // 1. the game's picture into this frame's source image (the same bytes: the backend's images are 32-bit RGBA, the
    //    frames it makes come back into B8G8R8A8 the same way), once the game's copy is done and the passes before
    //    no longer read it
    begin(0);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffset = {sx, sy, 0},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
    vk.vkCmdCopyImage(G.cb[0], src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, G.im.source[index % 2], VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    vk.vkEndCommandBuffer(G.cb[0]);
    VkSemaphoreSubmitInfo w0[2] = {
        {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait_sem, .value = wait_value, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
        {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = G.im.sync, .value = s ? s - 1 : 0, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    if (!wait_sem) w0[0] = w0[1];
    if (!submit(0, w0, wait_sem ? 2 : 1, G.im.sync, s)) { stop(want, "its copy was refused"); return 0; }
    // 2. the passes
    vk_queue_lock();
    int r = lsfg_schedule(err, sizeof err);
    vk_queue_unlock();
    if (r) { stop(want, err); return 0; }
    int n = G.im.ndest < want ? G.im.ndest : want;
    G.made += (uint64_t)n;
    if (!index) return 0;   // (the first frame: nothing before it)
    // 3. the made frames out, into the present thread's images, once the last is made
    begin(1);
    for (int i = 0; i < n; i++) {
        barrier(G.cb[1], G.img[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vk.vkCmdCopyImage(G.cb[1], G.im.dest[i], VK_IMAGE_LAYOUT_GENERAL, G.img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(G.cb[1], G.img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    }
    vk.vkEndCommandBuffer(G.cb[1]);
    VkSemaphoreSubmitInfo w1 = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = G.im.sync, .value = s + (uint64_t)n,
                                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    if (!submit(1, &w1, 1, G.done, G.done_value + 1)) { stop(want, "its frames' copy was refused"); return 0; }
    G.done_value++;
    for (int i = 0; i < n; i++)
        out[i] = (tsword_fg_image){G.img[i], (uint32_t)sw, (uint32_t)sh, G.done, G.done_value};
    G.shown += (uint64_t)n, G.rate_n += (uint64_t)n;
    return n;
}
