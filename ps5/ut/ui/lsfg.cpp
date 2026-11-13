// Frame generation with the player's own Lossless.dll (TSword PS5, from WSword): lsfg.h's calls on
// ps5/third_party/lsfg-vk's backend.
// SPDX-License-Identifier: GPL-3.0-or-later (it links lsfg-vk, GPL-3.0: README.TSword.md there)
#include "lsfg.h"

#include "lsfg-vk-backend/lsfgvk.hpp"

#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace lb = lsfgvk::backend;

namespace {
std::unique_ptr<lb::Instance> g_inst;
lb::Context *g_ctx;

// (the backend's messages are "what\n- why\n- why": one line for the log and the settings page)
void put(char *err, size_t n, const char *what, const char *why) {
    if (!err || !n) return;
    std::string s = why ? why : "";
    for (char &c : s) if (c == '\n') c = ' ';
    snprintf(err, n, "%s%s%s", what, s.empty() ? "" : ": ", s.c_str());
}
}  // namespace

extern "C" int lsfg_check_dll(const char *path, bool *fp16, char *err, size_t n) {
    try {
        int sets = lb::checkShaderDll(path);
        if (fp16) *fp16 = (sets & 2) != 0;
        if (!(sets & 1)) {  // (the FP16 set only: a device without shaderFloat16 can't use it)
            put(err, n, "only the FP16 shaders are in this Lossless.dll", nullptr);
        }
        return 0;
    } catch (const std::exception &e) {
        put(err, n, "Lossless.dll", e.what());
    } catch (...) {
        put(err, n, "Lossless.dll", "unknown error");
    }
    return -1;
}

extern "C" int lsfg_init(const LsfgDevice *d, const char *dll, bool allow_fp16, char *err, size_t n) {
    lsfg_shutdown();
    lb::ExternalDevice ed{};
    ed.instance = d->instance;
    ed.physdev = d->phys;
    ed.device = d->dev;
    ed.queueFamily = d->qfam;
    ed.queue = d->queue;
    ed.getInstanceProcAddr = d->gipa;
    ed.fp16 = d->fp16;
    if (d->cache_path) ed.cacheFile = d->cache_path;
    // FP16 shaders first where allowed (Lossless Scaling's own default), full precision if they can't be built
    for (int attempt = allow_fp16 && d->fp16 ? 0 : 1; attempt < 2; attempt++) {
        try {
            g_inst = std::make_unique<lb::Instance>(ed, dll, attempt == 0);
            return 0;
        } catch (const std::exception &e) {
            put(err, n, attempt == 0 ? "frame generation (FP16 shaders)" : "frame generation", e.what());
        } catch (...) {
            put(err, n, "frame generation", "unknown error");
        }
        g_inst.reset();
    }
    return -1;
}

extern "C" bool lsfg_ready(void) { return g_inst != nullptr; }
extern "C" bool lsfg_low_precision(void) { return g_inst && g_inst->lowPrecision(); }
extern "C" bool lsfg_is_open(void) { return g_ctx != nullptr; }

extern "C" void lsfg_close(void) {
    if (!g_inst || !g_ctx) return;
    try {
        g_inst->closeContext(*g_ctx);
    } catch (...) {
    }
    g_ctx = nullptr;
}

extern "C" int lsfg_open(int generated, uint32_t w, uint32_t h, float flow_scale, bool perf, LsfgImages *out, char *err, size_t n) {
    lsfg_close();
    if (!g_inst) { put(err, n, "frame generation", "not started"); return -1; }
    if (generated < 1 || generated > LSFG_MAX_GENERATED) { put(err, n, "frame generation", "unsupported multiplier"); return -1; }
    if (flow_scale < 0.25f) flow_scale = 0.25f;
    if (flow_scale > 1.0f) flow_scale = 1.0f;
    try {
        // (the backend's flow factor divides the size: 1 / Lossless Scaling's flow scale, as lsfg-vk's layer passes it)
        g_ctx = &g_inst->openLocalContext((size_t)generated, w, h, false, 1.0f / flow_scale, perf);
        lb::ContextImages im = lb::Instance::images(*g_ctx);
        *out = LsfgImages{};
        out->source[0] = im.source[0];
        out->source[1] = im.source[1];
        out->ndest = (int)im.dest.size();
        for (int i = 0; i < out->ndest && i < LSFG_MAX_GENERATED; i++) {
            out->dest[i] = im.dest[(size_t)i];
            out->dest_view[i] = im.destViews[(size_t)i];
        }
        out->sync = im.sync;
        out->format = im.format;
        out->width = w;
        out->height = h;
        g_inst->persistCache();
        return 0;
    } catch (const std::exception &e) {
        put(err, n, "frame generation context", e.what());
    } catch (...) {
        put(err, n, "frame generation context", "unknown error");
    }
    g_ctx = nullptr;
    return -1;
}

extern "C" uint64_t lsfg_next_signal(void) { return g_ctx ? lb::Instance::nextSignal(*g_ctx) : 0; }
extern "C" uint64_t lsfg_frame_index(void) { return g_ctx ? lb::Instance::frameIndex(*g_ctx) : 0; }

extern "C" int lsfg_schedule(char *err, size_t n) {
    if (!g_inst || !g_ctx) { put(err, n, "frame generation", "no context"); return -1; }
    try {
        g_inst->scheduleFrames(*g_ctx);
        return 0;
    } catch (const std::exception &e) {
        put(err, n, "frame generation", e.what());
    } catch (...) {
        put(err, n, "frame generation", "unknown error");
    }
    return -1;
}

extern "C" int lsfg_schedule_some(const int *which, int count, char *err, size_t n) {
    if (!g_inst || !g_ctx) { put(err, n, "frame generation", "no context"); return -1; }
    try {
        std::vector<size_t> w;
        for (int k = 0; k < count; k++) w.push_back((size_t)which[k]);
        g_inst->scheduleFrames(*g_ctx, w);
        return 0;
    } catch (const std::exception &e) {
        put(err, n, "frame generation", e.what());
    } catch (...) {
        put(err, n, "frame generation", "unknown error");
    }
    return -1;
}

extern "C" void lsfg_shutdown(void) {
    lsfg_close();
    g_inst.reset();
}
