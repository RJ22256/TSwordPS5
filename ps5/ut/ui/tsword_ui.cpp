// (TSword PS5) The console menu and the performance overlay, as WSword PS5's (WSword runtime/ui/menu.mm: the console
// menu, perf_overlay_draw), drawn with Dear ImGui over the TV's picture by the present thread
// (ultratears/src/vk/present.c present_one calls tsword_ui_record after the game's frame is in the swapchain image).
//
//   The quick menu   a two-finger tap on the touch pad (ps5/ut/sdl/sdl_ps5.c): a sheet rising from the bottom over the
//                    dimmed game - Resume, Frame rate, Resolution, Frame generation, Performance overlay - and All
//                    settings / Quit. Circle or the tap again closes it.
//   All settings     the categories down the left (L1 / R1), their rows on the right (option_rows.cpp), the selected
//                    row's description along the foot. Circle goes back to the quick menu.
//   The overlay      Off, FPS (the frame rate and frame time) or Full (CPU, temperatures, memory, the frame times'
//                    graph, the resolution): perf_overlay= in ultratears.cfg.
//
// While the menu is open the game sees the controller at rest (sdl_ps5.c asks tsword_ui_captures). The settings are
// ultratears.cfg's, written with host/boot.c totk_cfg_set; the resolution, the frame rate and the others the game reads
// as it starts apply at the next start (the menu says so), the overlay and frame generation at once.
//
// SST (the PS5's typeface) is the console's own: read from its system fonts, never shipped. Without it, Inter (the
// runtime's, host/text_entry.c).
#include <atomic>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/resource.h>
#include <time.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_vulkan.h"

extern "C" {
#include "vk.h"   // (ultratears/src/vk: the device, the queue and its lock)
#include <SDL3/SDL_vulkan.h>
#include <ps5platform/memory.h>
int sceKernelGetCpuTemperature(int *celsius);
int sceKernelGetSocSensorTemperature(int sensor, int *celsius);
void totk_cfg_set(const char *key, const char *value);   // host/boot.c
void vkp_wake(void);                                      // vk/present.c
extern const unsigned char te_inter_ttf[], te_inter_ttf_end[];   // host/text_entry.c (Inter, compiled in)
// frame generation (ps5/ut/ui/framegen.cpp when it is built in): weak, 0 without it
__attribute__((weak)) int tsword_fg_available(void);
__attribute__((weak)) int tsword_fg_generated_fps(float *fps);
__attribute__((weak)) const char *tsword_fg_status(void);
}

#include "option_rows.h"

#define LOG(...) fprintf(stderr, "[tsword-ui] " __VA_ARGS__)

// ------------------------------------------------------------------ the settings (ultratears.cfg)
namespace {
struct Choice { const char *value, *name; };
const Choice k_res[] = {{"1920x1080", "1080p"}, {"2560x1440", "1440p"}, {"3840x2160", "4K"}};
const Choice k_scale[] = {{"50", "50%"}, {"59", "59%"}, {"67", "67%"}, {"77", "77%"}, {"", "83% (the game's)"}, {"100", "100%"}};
const Choice k_fps[] = {{"30", "30"}, {"40", "40"}, {"45", "45"}, {"60", "60"}};
const Choice k_upscale[] = {{"", "FSR 1"}, {"bilinear", "Bilinear"}};
const Choice k_aa[] = {{"", "The game's FXAA"}, {"smaa", "SMAA"}, {"off", "Off"}};
const Choice k_sharp[] = {{"0", "Off"}, {"25", "25%"}, {"", "50%"}, {"75", "75%"}, {"100", "100%"}};
const Choice k_layout[] = {{"xbox", "Cross confirms (PlayStation)"}, {"nintendo", "Circle confirms (Switch)"}};
const Choice k_overlay[] = {{"", "Off"}, {"fps", "FPS"}, {"full", "Full"}};
const Choice k_fg[] = {{"", "Off"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}};
const Choice k_speed[] = {{"safe", "Safe"}, {"", "Balanced"}, {"full", "Full"}};
const Choice k_preempt[] = {{"", "Off"}, {"1", "On (experimental)"}};
template <size_t N> constexpr int count(const Choice (&)[N]) { return (int)N; }

struct Setting { const char *key; const Choice *opts; int n, def; bool live; int start, cur; };
enum { S_RES, S_SCALE, S_FPS, S_UPSCALE, S_AA, S_SHARP, S_LAYOUT, S_OVERLAY, S_FG, S_SPEED, S_PREEMPT, S_COUNT };
Setting g_set[S_COUNT] = {
    {"resolution", k_res, count(k_res), 0, false, 0, 0},
    {"scale", k_scale, count(k_scale), 4, false, 0, 0},
    {"fps", k_fps, count(k_fps), 0, false, 0, 0},
    {"upscale", k_upscale, count(k_upscale), 0, false, 0, 0},
    {"antialias", k_aa, count(k_aa), 0, false, 0, 0},
    {"sharpness", k_sharp, count(k_sharp), 2, false, 0, 0},
    {"pad_layout", k_layout, count(k_layout), 0, false, 0, 0},
    {"perf_overlay", k_overlay, count(k_overlay), 0, true, 0, 0},
    {"framegen", k_fg, count(k_fg), 0, true, 0, 0},
    {"speedups", k_speed, count(k_speed), 1, false, 0, 0},
    {"TOTK_ASYNC_PREEMPT", k_preempt, count(k_preempt), 0, false, 0, 0},
};
std::atomic<int> g_overlay_mode{0}, g_fg_mode{0};   // (read by other threads: the overlay, frame generation)

int index_of(const Setting &s, const char *v) {
    for (int i = 0; i < s.n; i++) if (!strcmp(s.opts[i].value, v)) return i;
    return -1;
}

// ultratears.cfg as the game read it (the title folder's: ps5_cwd.c); the PS5's defaults where it says nothing
void settings_load(void) {
    for (Setting &s : g_set) s.cur = s.def;
    if (const char *e = getenv("TOTK_FPS_OVERLAY")) if (*e && strcmp(e, "0")) g_set[S_OVERLAY].cur = 1;   // (fps_overlay=1: the old counter)
    if (FILE *f = fopen("ultratears.cfg", "r")) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (line[0] == '#' || !eq) continue;
            *eq = 0;
            for (Setting &s : g_set) {
                if (strcmp(line, s.key)) continue;
                int i = index_of(s, eq + 1);
                if (i >= 0) s.cur = i;
                else if (&s == &g_set[S_FPS] && atoi(eq + 1) > 45) s.cur = 3;   // (120, display...: the console shows 60)
                else LOG("%s=%s is not one of the menu's values: shown as %s\n", s.key, eq + 1, s.opts[s.cur].name);
            }
        }
        fclose(f);
    }
    for (Setting &s : g_set) s.start = s.cur;
    g_overlay_mode = g_set[S_OVERLAY].cur, g_fg_mode = g_set[S_FG].cur;
    LOG("perf_overlay %s, framegen %s; the menu: a two-finger tap on the touch pad\n", k_overlay[g_overlay_mode.load()].name,
        k_fg[g_fg_mode.load()].name);
}

void setting_store(int which, int v) {
    Setting &s = g_set[which];
    if (v < 0 || v >= s.n || v == s.cur) return;
    s.cur = v;
    totk_cfg_set(s.key, s.opts[v].value);
    if (which == S_OVERLAY) {
        g_overlay_mode = v;
        totk_cfg_set("fps_overlay", "");   // (the old 5x7 counter's switch: the overlay replaces it)
    }
    if (which == S_FG) {
        g_fg_mode = v;
        // (0.5.4) frame generation wants the game held to half the TV's 60: at 30 its frames come evenly and the made
        // ones fall between them (a steady 60 on the TV); at a limit the game cannot hold (40-55 in heavy places)
        // they come unevenly, more than the TV shows, and the picture judders. So turning it on sets 30, at the next
        // start (the frame rate row and tile show it; another limit can be chosen again).
        if (v && g_set[S_FPS].cur != 0) setting_store(S_FPS, 0);
    }
    LOG("%s=%s%s\n", s.key, s.opts[v].value, s.live ? "" : " (at the next start)");
}

// the settings changed this session that wait for the next start, by name ("" none)
std::string pending_restart(void) {
    std::string t;
    static const char *const names[S_COUNT] = {"resolution", "3D resolution", "frame rate", "upscaler", "anti-aliasing",
                                               "sharpness", "button layout", "", "", "speed-ups", "thread preemption"};
    for (int i = 0; i < S_COUNT; i++)
        if (!g_set[i].live && g_set[i].cur != g_set[i].start) t += (t.empty() ? "" : ", ") + std::string(names[i]);
    return t;
}
}  // namespace

// ------------------------------------------------------------------ the controller (from sdl_ps5.c's thread)
namespace {
std::atomic<bool> g_open{false};       // the menu has the controller (opened by the tap, closed when its sheet has sunk)
std::atomic<bool> g_close_req{false};  // the tap again: close with the closing animation
std::atomic<uint32_t> g_held{0}, g_pressed{0};   // SDL gamepad buttons down, and those pressed since the last frame
std::atomic<int32_t> g_lx{0}, g_ly{0};
}

extern "C" int tsword_ui_captures(void) { return g_open.load(); }
extern "C" int tsword_ui_fg_mode(void) { return g_fg_mode.load(); }   // (framegen.c: the frames made between two of the game's)
extern "C" void tsword_ui_tap(void) {
    if (!g_open.load()) {
        g_held = 0, g_pressed = 0;
        g_open = true;
        LOG("menu opened\n");
    } else g_close_req = true;
    vkp_wake();
}
extern "C" void tsword_ui_pad(uint32_t held, int16_t lx, int16_t ly) {
    uint32_t was = g_held.exchange(held);
    g_pressed.fetch_or(held & ~was);
    g_lx = lx, g_ly = ly;
}

// ------------------------------------------------------------------ the performance numbers
namespace {
enum { HISTORY = 120 };
struct Perf {
    float fps = 0, frame_ms = 0, frame_ms_max = 0, frame_ms_hist[HISTORY] = {};
    float cpu_pct = -1, cpu_temp = -1, soc_temp = -1, ram_used_gb = -1, ram_total_gb = -1;
    uint32_t src_w = 0, src_h = 0, out_w = 0, out_h = 0;
} g_perf;

double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

// At every new game frame shown (the present thread): its interval, the frame rate over half a second; the CPU,
// temperatures and memory once a second.
void perf_frame(void) {
    static double last, win_t0, sys_t0, cpu_last = -1;
    static int win_frames, hist_i;
    static float win_max;
    double t = now_s();
    if (last > 0) {
        float ms = (float)((t - last) * 1e3);
        if (ms < 1000) {
            g_perf.frame_ms_hist[hist_i] = ms, hist_i = (hist_i + 1) % HISTORY;
            win_max = ms > win_max ? ms : win_max;
        }
    }
    last = t;
    if (!win_t0) win_t0 = t;
    win_frames++;
    if (t - win_t0 >= 0.5) {
        g_perf.fps = (float)(win_frames / (t - win_t0));
        g_perf.frame_ms = (float)((t - win_t0) * 1e3 / win_frames);
        g_perf.frame_ms_max = win_max;
        win_t0 = t, win_frames = 0, win_max = 0;
    }
    if (t - sys_t0 < 1.0) return;
    double dt = sys_t0 ? t - sys_t0 : 0;
    sys_t0 = t;
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {   // (TSword's own time over the 13 threads a title has)
        double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
        if (cpu_last >= 0 && dt > 0) {
            float pct = (float)(100.0 * (cpu - cpu_last) / (dt * 13));
            g_perf.cpu_pct = pct > 100 ? 100 : pct;
        }
        cpu_last = cpu;
    }
    int c = 0;
    if (sceKernelGetCpuTemperature(&c) == 0 && c > 0 && c < 125) g_perf.cpu_temp = (float)c;
    c = 0;
    if (sceKernelGetSocSensorTemperature(0, &c) == 0 && c > 0 && c < 125) g_perf.soc_temp = (float)c;
    struct ps5_memory_stats ms;
    if (ps5_memory_query(&ms)) {
        g_perf.ram_total_gb = (float)(ms.total_bytes / 1073741824.0);
        g_perf.ram_used_gb = (float)((ms.total_bytes - ms.free_bytes) / 1073741824.0);
    }
}
}  // namespace

// ------------------------------------------------------------------ fonts, style, the controller's glyphs
namespace {
ImFont *g_sst, *g_sst_med, *g_sst_light;

void load_fonts(float k) {   // k: the TV's height / 1080 (the fonts made at the size they're seen at)
    ImGuiIO &io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    static const char *const dirs[] = {"fonts", "/app0/fonts", "/preinst/common/font", "/system/common/font",
                                       "/system_ex/common_ex/font"};
    auto find = [&](std::initializer_list<const char *> names, char *out, size_t n) {
        for (const char *d : dirs)
            for (const char *f : names) {
                snprintf(out, n, "%s/%s", d, f);
                if (FILE *fp = fopen(out, "rb")) { fclose(fp); return true; }
            }
        return false;
    };
    char rp[300], mp[300], lp[300];
    bool r = find({"SSTRg.TTF", "SSTRg.ttf", "SST-Roman.otf", "SST-Roman.ttf", "SSTRoman.ttf"}, rp, sizeof rp);
    bool m = find({"SSTMedium.TTF", "SSTMedium.ttf", "SST-Medium.otf", "SST-Medium.ttf"}, mp, sizeof mp);
    bool l = find({"SSTLight.TTF", "SSTLight.ttf", "SST-Light.otf", "SST-Light.ttf"}, lp, sizeof lp);
    if (r) g_sst = io.Fonts->AddFontFromFileTTF(rp, 30 * k, &cfg);
    if (m) g_sst_med = io.Fonts->AddFontFromFileTTF(mp, 30 * k, &cfg);
    if (l) g_sst_light = io.Fonts->AddFontFromFileTTF(lp, 58 * k, &cfg);
    else if (r) g_sst_light = io.Fonts->AddFontFromFileTTF(rp, 58 * k, &cfg);
    if (!g_sst || !g_sst_light) {   // Inter (the runtime's own copy: the atlas only reads it)
        ImFontConfig c = cfg;
        c.FontDataOwnedByAtlas = false;
        int n = (int)(te_inter_ttf_end - te_inter_ttf);
        if (!g_sst) g_sst = io.Fonts->AddFontFromMemoryTTF((void *)te_inter_ttf, n, 30 * k, &c);
        if (!g_sst_light) g_sst_light = io.Fonts->AddFontFromMemoryTTF((void *)te_inter_ttf, n, 58 * k, &c);
    }
    if (!g_sst_med) g_sst_med = g_sst;
    LOG("fonts: %s%s%s\n", r ? rp : "Inter (no SST on this console)", m ? ", SST medium" : "", l ? ", SST light" : "");
}

void style_init(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 14, s.FrameRounding = 10, s.ChildRounding = 12, s.PopupRounding = 18;
    s.WindowPadding = ImVec2(28, 24), s.FramePadding = ImVec2(14, 8), s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0;
}

void ps_glyph(ImDrawList *dl, ImVec2 c, float r, int kind, float scale) {   // 0 cross, 1 circle, 2 square, 3 triangle
    const ImU32 col = IM_COL32(236, 242, 248, 255);
    float w = 2.2f * scale, k = r * 0.55f;
    dl->AddCircle(c, r, IM_COL32(236, 242, 248, 110), 32, 1.4f * scale);
    if (kind == 0) {
        dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), col, w);
        dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), col, w);
    } else if (kind == 1) dl->AddCircle(c, k, col, 24, w);
    else if (kind == 2) dl->AddRect(ImVec2(c.x - k * 0.85f, c.y - k * 0.85f), ImVec2(c.x + k * 0.85f, c.y + k * 0.85f), col, 0, 0, w);
    else dl->AddTriangle(ImVec2(c.x, c.y - k), ImVec2(c.x - k, c.y + k * 0.7f), ImVec2(c.x + k, c.y + k * 0.7f), col, w);
}
float ps_pill(ImDrawList *dl, ImVec2 a, float h, const char *text, float scale) {   // L1 / R1; its width
    float fs = h * 0.62f, tw = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, text).x, w = tw + h * 0.9f;
    dl->AddRect(a, ImVec2(a.x + w, a.y + h), IM_COL32(236, 242, 248, 200), h * 0.3f, 0, 1.6f * scale);
    dl->AddText(ImGui::GetFont(), fs, ImVec2(a.x + (w - tw) * 0.5f, a.y + (h - fs) * 0.5f - 1 * scale), IM_COL32(236, 242, 248, 255), text);
    return w;
}
}  // namespace

// ------------------------------------------------------------------ the performance overlay (WSword PS5's look)
namespace {
void perf_cell(const char *fmt, float v) {   // (v < 0: not known, a dash)
    ImGui::TableNextColumn();
    if (v < 0) ImGui::TextDisabled("-");
    else ImGui::Text(fmt, v);
}
void perf_blank(void) { ImGui::TableNextColumn(); }

void perf_overlay_draw(ImGuiIO &io, float scale, int mode) {
    const Perf &s = g_perf;
    float gen = 0;
    if (tsword_fg_generated_fps) tsword_fg_generated_fps(&gen);
    // a black card, a thin grey edge, SST, grey labels and white values, fading in (top right: the game keeps its
    // hearts and stamina top left)
    static int seen_frame = -10, seen_mode = -1;
    static double shown_at;
    int frame = ImGui::GetFrameCount();
    if (frame - seen_frame > 1 || seen_mode != mode) shown_at = ImGui::GetTime();
    seen_frame = frame, seen_mode = mode;
    float fe = fminf(1.0f, (float)(ImGui::GetTime() - shown_at) / 0.25f);
    fe = 1 - (1 - fe) * (1 - fe) * (1 - fe);
    float m = 28 * scale;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - m, m - 6 * scale * (1 - fe)), 0, ImVec2(1, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fe);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * scale, 12 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8 * scale, 2 * scale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(10 / 255.f, 10 / 255.f, 11 / 255.f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(46 / 255.f, 46 / 255.f, 52 / 255.f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(242 / 255.f, 242 / 255.f, 243 / 255.f, 1));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(124 / 255.f, 124 / 255.f, 132 / 255.f, 1));
    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(242 / 255.f, 242 / 255.f, 243 / 255.f, 1));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(28 / 255.f, 28 / 255.f, 31 / 255.f, 1));
    ImGui::PushFont(g_sst);   // (outside the window: End() checks the font stack it began with)
    struct PerfStyleEnd {
        ~PerfStyleEnd() { ImGui::PopFont(), ImGui::PopStyleColor(6), ImGui::PopStyleVar(5); }
    } perf_style_end;   // (popped after End(), on every return)
    ImGui::Begin("##perf", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    if (mode == 1) {   // FPS: the number, and FPS and the frame time in grey beside it
        ImGui::PushFont(g_sst_med);
        ImGui::SetWindowFontScale(1.05f);
        ImGui::Text("%.0f", s.fps + gen);
        ImGui::PopFont();
        ImGui::SameLine(0, 8 * scale);
        ImGui::SetWindowFontScale(0.66f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5 * scale);
        ImGui::TextDisabled("FPS   %.1f ms", s.frame_ms);
        ImGui::End();
        return;
    }
    ImGui::SetWindowFontScale(0.78f);
    const ImVec4 lab(142 / 255.f, 142 / 255.f, 150 / 255.f, 1);
    if (ImGui::BeginTable("##perft", 4, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableNextColumn(); ImGui::TextColored(lab, "GPU");
        perf_cell("%.0f C", s.soc_temp); perf_blank(); perf_blank();
        ImGui::TableNextColumn(); ImGui::TextColored(lab, "CPU");
        perf_cell("%.0f%%", s.cpu_pct); perf_cell("%.0f C", s.cpu_temp); perf_blank();
        ImGui::TableNextColumn(); ImGui::TextColored(lab, "RAM");
        perf_cell("%.1f GB", s.ram_used_gb); perf_cell("/ %.1f GB", s.ram_total_gb); perf_blank();
        ImGui::TableNextColumn(); ImGui::TextColored(lab, "FPS");
        perf_cell("%.0f", s.fps + gen); perf_cell("%.1f ms", s.frame_ms); perf_cell("max %.1f ms", s.frame_ms_max);
        ImGui::EndTable();
    }
    float top = s.frame_ms_max > 50 ? s.frame_ms_max : 50;
    ImGui::PlotLines("##ft", s.frame_ms_hist, HISTORY, 0, NULL, 0, top, ImVec2(330 * scale, 48 * scale));
    ImGui::TextDisabled("PlayStation 5 (GPU temperature: the SoC's)");
    char line[160];
    int up = g_set[S_UPSCALE].start, aa = g_set[S_AA].start;
    if (s.src_w && s.out_w)
        snprintf(line, sizeof line, "%u x %u -> %u x %u | %s | %s", s.src_w, s.src_h, s.out_w, s.out_h,
                 s.out_w > s.src_w ? k_upscale[up].name : "No upscaling", aa == 0 ? "FXAA" : aa == 1 ? "SMAA" : "No AA");
    else snprintf(line, sizeof line, "%s", "-");
    ImGui::TextDisabled("%s", line);
    if (gen > 0) ImGui::TextDisabled("Frame generation %s: %.0f generated frames a second", k_fg[g_fg_mode.load()].name, gen);
    ImGui::End();
}
}  // namespace

// ------------------------------------------------------------------ the console menu (WSword PS5's)
namespace {
enum { CV_QUICK, CV_SETTINGS };
int g_cview;
double g_open_at, g_view_at, g_close_at = -1;
bool g_was_open;
int g_ccat;               // the open category (k_ccats)
float g_cbar = -1;        // the categories' bar (its y, gliding)
ImVec4 g_ring(-1, 0, 0, 0);  // the quick menu's ring (x0, y0, x1, y1), gliding
int g_cfocus = 0;         // frames left to put the cursor on g_cfocus_on
int g_cfocus_on;          // 0: Resume, 5: All settings
double g_help_at;         // when the foot's description last changed
std::string g_help_name;
bool g_quit_ask;
const char *const k_ccats[] = {"Video", "Performance", "Controls", "Quit"};
enum { NCCATS = sizeof k_ccats / sizeof *k_ccats };

float c_ease(float x) { x = x < 0 ? 0 : x > 1 ? 1 : x; float u = 1 - x; return 1 - u * u * u; }
ImU32 c_fade(ImU32 c, float a) {
    int al = (int)((c >> IM_COL32_A_SHIFT & 0xff) * (a < 0 ? 0 : a > 1 ? 1 : a) + 0.5f);
    return (c & ~IM_COL32_A_MASK) | ((ImU32)al << IM_COL32_A_SHIFT);
}
float c_approach(float cur, float target, float rate = 16) {
    float n = cur + (target - cur) * (1 - expf(-ImGui::GetIO().DeltaTime * rate));
    return fabsf(n - target) < 0.05f ? target : n;
}
ImFont *c_reg(void) { return g_sst; }
ImFont *c_med(void) { return g_sst_med; }
ImFont *c_light(void) { return g_sst_light; }
void c_text(ImDrawList *dl, ImFont *f, float size, ImVec2 at, ImU32 col, const char *t) { dl->AddText(f, size, at, col, t); }
float c_width(ImFont *f, float size, const char *t) { return f->CalcTextSizeA(size, FLT_MAX, 0, t).x; }

// the two-finger tap's mark: the touch pad with two dots
void c_tap_glyph(ImDrawList *dl, ImVec2 c, float s, ImU32 col) {
    float w = 15 * s, h = 10 * s;
    dl->AddRect(ImVec2(c.x - w, c.y - h), ImVec2(c.x + w, c.y + h), col, 4 * s, 0, 1.5f * s);
    dl->AddCircleFilled(ImVec2(c.x - 4.5f * s, c.y), 2.6f * s, col, 12);
    dl->AddCircleFilled(ImVec2(c.x + 4.5f * s, c.y), 2.6f * s, col, 12);
}
// prompts from the right edge: {glyph (0 cross, 1 circle, -1 pill, -2 the tap), pill text, label}
struct CPrompt { int glyph; const char *pill, *label; };
void c_prompts(ImDrawList *dl, float right, float cy, float s, const CPrompt *p, int n, float a) {
    float fs = 19 * s, x = right;
    ImFont *f = c_reg();
    ImU32 txt = c_fade(IM_COL32(200, 200, 206, 255), a);
    for (int i = 0; i < n; i++) {
        const char *l = p[i].label;
        x -= c_width(f, fs, l);
        dl->AddText(f, fs, ImVec2(x, cy - fs * 0.55f), txt, l);
        x -= 10 * s;
        if (p[i].glyph >= 0) { x -= 11 * s; ps_glyph(dl, ImVec2(x, cy), 11 * s, p[i].glyph, s); x -= 11 * s; }
        else if (p[i].glyph == -2) { x -= 15 * s; c_tap_glyph(dl, ImVec2(x, cy), s, txt); x -= 15 * s; }
        else {
            float ph = 24 * s, pw = ImGui::GetFont()->CalcTextSizeA(ph * 0.62f, FLT_MAX, 0, p[i].pill).x + ph * 0.9f;
            x -= pw;
            ps_pill(dl, ImVec2(x, cy - ph * 0.5f), ph, p[i].pill, s);
        }
        x -= 30 * s;
    }
}

void c_close(void) { if (g_close_at < 0) g_close_at = ImGui::GetTime(); }
void c_open_settings(int cat) {
    g_cview = CV_SETTINGS, g_view_at = ImGui::GetTime(), g_ccat = cat;
    g_quit_ask = false;
    opt_page_replay();
    opt_focus_first(true);
}
void c_back_to_quick(void) {
    g_cview = CV_QUICK, g_view_at = ImGui::GetTime();
    g_cfocus = 4, g_cfocus_on = 5;   // (the cursor back on All settings)
}

// A switch, as the rows': k 0 off .. 1 on
void c_switch(ImDrawList *dl, ImVec2 right_mid, float s, float k, float a) {
    float sw = 52 * s, sh = 30 * s, x0 = right_mid.x - sw, cy = right_mid.y;
    ImVec4 t0 = ImGui::ColorConvertU32ToFloat4(IM_COL32(46, 46, 51, 255)), t1(0.95f, 0.95f, 0.953f, 1);
    ImU32 track = ImGui::ColorConvertFloat4ToU32(ImVec4(t0.x + (t1.x - t0.x) * k, t0.y + (t1.y - t0.y) * k, t0.z + (t1.z - t0.z) * k, a));
    dl->AddRectFilled(ImVec2(x0, cy - sh * 0.5f), ImVec2(right_mid.x, cy + sh * 0.5f), track, sh * 0.5f);
    float kr = sh * 0.5f - 3 * s, kx = x0 + 3 * s + kr + (sw - 6 * s - 2 * kr) * k;
    float g = 0.56f - 0.52f * k;
    dl->AddCircleFilled(ImVec2(kx, cy), kr, ImGui::ColorConvertFloat4ToU32(ImVec4(g, g, g + 0.03f, a)), 24);
}
// A segmented choice: names[n], the pill gliding to cur (*pill: its animated position)
void c_segments(ImDrawList *dl, ImVec2 a0, ImVec2 a1, float s, const char *const *names, int n, int cur, float *pill, float a) {
    dl->AddRectFilled(a0, a1, c_fade(IM_COL32(12, 12, 14, 255), a), 9 * s);
    float w = (a1.x - a0.x - 6 * s) / n;
    if (cur >= 0) {
        *pill = *pill < 0 ? (float)cur : c_approach(*pill, (float)cur, 14);
        float px = a0.x + 3 * s + w * *pill;
        dl->AddRectFilled(ImVec2(px, a0.y + 3 * s), ImVec2(px + w, a1.y - 3 * s), c_fade(IM_COL32(46, 46, 51, 255), a), 7 * s);
    }
    float fs = 18 * s;
    for (int i = 0; i < n; i++) {
        const char *t = names[i];
        float tw = c_width(c_reg(), fs, t), cx = a0.x + 3 * s + w * (i + 0.5f);
        dl->AddText(c_reg(), fs, ImVec2(cx - tw * 0.5f, (a0.y + a1.y) * 0.5f - fs * 0.55f),
                    c_fade(i == cur ? IM_COL32(242, 242, 243, 255) : IM_COL32(142, 142, 150, 255), a), t);
    }
}

bool fg_available(void) { return tsword_fg_available && tsword_fg_available(); }

void c_quick(ImGuiIO &io, float s, float closing) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    double now = ImGui::GetTime(), base = g_cview == CV_QUICK && g_view_at > g_open_at ? g_view_at : g_open_at;
    float k = c_ease((float)(now - base) / 0.38f), a = k * (1 - closing);
    float W = io.DisplaySize.x, H = io.DisplaySize.y;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, c_fade(IM_COL32(0, 0, 0, 150), a));
    float dy = 44 * s * (1 - k) + 28 * s * c_ease(closing);
    float x0 = 64 * s, x1 = W - 64 * s, y1 = H - 48 * s + dy, h = 336 * s, y0 = y1 - h, pad = 34 * s;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), c_fade(IM_COL32(10, 10, 11, 242), a), 22 * s);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), c_fade(IM_COL32(38, 38, 42, 255), a), 22 * s, 0, 1.2f * s);
    // the title, and what waits for the next start (else: changes apply at once)
    float ts = 40 * s;
    c_text(dl, c_light(), ts, ImVec2(x0 + pad, y0 + 26 * s), c_fade(IM_COL32(242, 242, 243, 255), a), "Quick menu");
    float tw = c_width(c_light(), ts, "Quick menu");
    c_text(dl, c_med(), 15 * s, ImVec2(x0 + pad + tw + 18 * s, y0 + 26 * s + ts * 0.55f), c_fade(IM_COL32(142, 142, 150, 255), a), "TSWORD");
    std::string pend = pending_restart();
    std::string note = pend.empty() ? "Changes apply at once" : "At the next start: " + pend;
    c_text(dl, c_reg(), 18 * s, ImVec2(x1 - pad - c_width(c_reg(), 18 * s, note.c_str()), y0 + 26 * s + ts * 0.42f),
           c_fade(IM_COL32(161, 161, 168, 255), a), note.c_str());
    // the tiles
    int fg = g_set[S_FG].cur;
    static int fg_last = 1;   // (frame generation turned back on: the last one used this session, else 2x)
    if (fg) fg_last = fg;
    bool fg_ok = fg_available();
    static float knob_fg = -1, pill_fps = -1, pill_res = -1, pill_ov = -1;
    if (knob_fg < 0) knob_fg = fg != 0;
    knob_fg = c_approach(knob_fg, fg && fg_ok ? 1.0f : 0.0f);
    float gap = 14 * s, ty0 = y0 + 96 * s, th = 150 * s, tw5 = (x1 - x0 - 2 * pad - 4 * gap) / 5;
    ImVec4 target = g_ring;
    bool any_hot = false;
    for (int i = 0; i < 5; i++) {
        float e = c_ease((float)(now - base - 0.06 - i * 0.04) / 0.34f) * (1 - closing), tdy = 12 * s * (1 - e);
        ImVec2 a0(x0 + pad + i * (tw5 + gap), ty0 + tdy), a1(a0.x + tw5, a0.y + th);
        ImGui::SetCursorScreenPos(ImVec2(a0.x, ty0));
        ImGui::PushID(i);
        if (g_cfocus > 0 && g_cfocus_on == i) { ImGui::SetKeyboardFocusHere(); ImGui::SetNavCursorVisible(true); g_cfocus--; }
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
        bool clicked = ImGui::InvisibleButton("##tile", ImVec2(tw5, th), ImGuiButtonFlags_EnableNav);
        ImGui::PopStyleColor();
        bool hot = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
        ImGui::PopID();
        if (hot) target = ImVec4(a0.x, a0.y, a1.x, a1.y), any_hot = true;
        ImU32 bg = i == 0 ? IM_COL32(242, 242, 243, 255) : IM_COL32(24, 24, 27, 255);
        dl->AddRectFilled(a0, a1, c_fade(bg, e), 16 * s);
        float lx = a0.x + 20 * s, ly = a0.y + 20 * s, lfs = 17 * s;
        ImU32 label = c_fade(IM_COL32(161, 161, 168, 255), e), ink = c_fade(IM_COL32(242, 242, 243, 255), e);
        switch (i) {
        case 0: {   // Resume
            ImU32 black = c_fade(IM_COL32(0, 0, 0, 255), e);
            dl->AddTriangleFilled(ImVec2(lx, ly), ImVec2(lx, ly + 22 * s), ImVec2(lx + 18 * s, ly + 11 * s), black);
            c_text(dl, c_med(), 24 * s, ImVec2(lx, a1.y - 20 * s - 26 * s), black, "Resume");
            if (clicked) c_close();
            break;
        }
        case 1: {   // Frame rate: 30 / 40 / 60 (45 from the settings: no pill)
            static const char *const fn[] = {"30", "40", "60"};
            static const int idx[] = {0, 1, 3};
            int cur = g_set[S_FPS].cur, seg = cur == 0 ? 0 : cur == 1 ? 1 : cur == 3 ? 2 : -1;
            c_text(dl, c_reg(), lfs, ImVec2(lx, ly), label, "Frame rate");
            c_segments(dl, ImVec2(lx, a1.y - 20 * s - 40 * s), ImVec2(a1.x - 20 * s, a1.y - 20 * s), s, fn, 3, seg, &pill_fps, e);
            if (clicked) setting_store(S_FPS, idx[(seg + 1) % 3]);
            break;
        }
        case 2: {   // Resolution: 1080p / 1440p / 4K
            static const char *const rn[] = {"1080p", "1440p", "4K"};
            c_text(dl, c_reg(), lfs, ImVec2(lx, ly), label, "Resolution");
            c_segments(dl, ImVec2(lx, a1.y - 20 * s - 40 * s), ImVec2(a1.x - 20 * s, a1.y - 20 * s), s, rn, 3, g_set[S_RES].cur, &pill_res, e);
            if (clicked) setting_store(S_RES, (g_set[S_RES].cur + 1) % 3);
            break;
        }
        case 3: {   // Frame generation (the last one used, else 2x)
            c_text(dl, c_reg(), lfs, ImVec2(lx, ly), label, "Frame generation");
            c_text(dl, c_reg(), 22 * s, ImVec2(lx, a1.y - 20 * s - 26 * s), fg_ok ? ink : label,
                   !fg_ok ? "Not available" : fg ? k_fg[fg].name : "Off");
            if (fg_ok) c_switch(dl, ImVec2(a1.x - 20 * s, ly + 15 * s + 30 * s), s, knob_fg, e);
            if (clicked && fg_ok) setting_store(S_FG, fg ? 0 : fg_last);
            break;
        }
        case 4: {   // Performance overlay
            static const char *const on[] = {"Off", "FPS", "Full"};
            c_text(dl, c_reg(), lfs, ImVec2(lx, ly), label, "Performance overlay");
            c_segments(dl, ImVec2(lx, a1.y - 20 * s - 40 * s), ImVec2(a1.x - 20 * s, a1.y - 20 * s), s, on, 3, g_set[S_OVERLAY].cur, &pill_ov, e);
            if (clicked) setting_store(S_OVERLAY, (g_set[S_OVERLAY].cur + 1) % 3);
            break;
        }
        }
    }
    // All settings and Quit, the prompts at the right
    float by = ty0 + th + 22 * s, bh = 50 * s, bx = x0 + pad;
    const char *labels[2] = {"All settings", "Quit"};
    for (int i = 0; i < 2; i++) {
        float e = c_ease((float)(now - base - 0.26 - i * 0.03) / 0.34f) * (1 - closing);
        float bw = c_width(c_reg(), 19 * s, labels[i]) + (i == 0 ? 60 : 40) * s;
        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        ImGui::PushID(10 + i);
        if (g_cfocus > 0 && g_cfocus_on == 5 + i) { ImGui::SetKeyboardFocusHere(); ImGui::SetNavCursorVisible(true); g_cfocus--; }
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
        bool clicked = ImGui::InvisibleButton("##btn", ImVec2(bw, bh), ImGuiButtonFlags_EnableNav);
        ImGui::PopStyleColor();
        bool hot = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
        ImGui::PopID();
        float bdy = 12 * s * (1 - e);
        ImVec2 a0(bx, by + bdy), a1(bx + bw, by + bh + bdy);
        if (hot) target = ImVec4(a0.x, a0.y, a1.x, a1.y), any_hot = true;
        dl->AddRectFilled(a0, a1, c_fade(IM_COL32(24, 24, 27, 255), e), 12 * s);
        dl->AddRect(a0, a1, c_fade(IM_COL32(44, 44, 49, 255), e), 12 * s, 0, 1.2f * s);
        c_text(dl, c_reg(), 19 * s, ImVec2(a0.x + 20 * s, (a0.y + a1.y) * 0.5f - 19 * s * 0.55f), c_fade(IM_COL32(242, 242, 243, 255), e), labels[i]);
        if (i == 0) {
            float cx = a1.x - 22 * s, cy = (a0.y + a1.y) * 0.5f, cs = 5 * s;
            dl->AddLine(ImVec2(cx - cs, cy - cs), ImVec2(cx, cy), c_fade(IM_COL32(142, 142, 150, 255), e), 2 * s);
            dl->AddLine(ImVec2(cx, cy), ImVec2(cx - cs, cy + cs), c_fade(IM_COL32(142, 142, 150, 255), e), 2 * s);
        }
        if (clicked) c_open_settings(i == 0 ? 0 : NCCATS - 1);
        bx += bw + 12 * s;
    }
    static const CPrompt pr[] = {{-2, NULL, "Close"}, {1, NULL, "Resume"}, {0, NULL, "Select"}};
    c_prompts(dl, x1 - pad, by + bh * 0.5f, s, pr, 3, a);
    // the ring: glides to the tile or button the cursor is on
    if (any_hot) {
        if (g_ring.x < 0) g_ring = target;
        g_ring.x = c_approach(g_ring.x, target.x, 18), g_ring.y = c_approach(g_ring.y, target.y, 18);
        g_ring.z = c_approach(g_ring.z, target.z, 18), g_ring.w = c_approach(g_ring.w, target.w, 18);
        float ra = c_ease((float)(now - base - 0.26) / 0.3f) * (1 - closing);
        ImVec2 r0(g_ring.x - 3 * s, g_ring.y - 3 * s), r1(g_ring.z + 3 * s, g_ring.w + 3 * s);
        dl->AddRect(ImVec2(r0.x - 4 * s, r0.y - 4 * s), ImVec2(r1.x + 4 * s, r1.y + 4 * s), c_fade(IM_COL32(242, 242, 243, 22), ra), 22 * s, 0, 6 * s);
        dl->AddRect(r0, r1, c_fade(IM_COL32(242, 242, 243, 255), ra), 18 * s, 0, 2.2f * s);
    }
}

// a choice row on one of the settings
void choice_row(int which, const char *label, const char *desc) {
    Setting &st = g_set[which];
    const char *names[8];
    for (int i = 0; i < st.n && i < 8; i++) names[i] = st.opts[i].name;
    int v = st.cur;
    if (opt_choice(label, &v, names, st.n, st.def, desc)) setting_store(which, v);
}

void page_video(float rs) {
    opt_begin("##video", rs);
    opt_section("Display");
    choice_row(S_RES, "Resolution", "The size of the picture the game makes: 1080p, 1440p or 4K. The TV keeps its own mode "
               "and the upscaler fits the picture to it. Larger pictures ask more of the GPU. Applies at the next start.");
    choice_row(S_SCALE, "3D resolution", "The game's 3D scene in % of the resolution; its menus and HUD stay sharp. The game's "
               "own is 83% (1600 x 900 at 1080p). Applies at the next start.");
    choice_row(S_UPSCALE, "Upscaler", "How the picture is fitted to the TV when the TV is larger than it: AMD FSR 1 (sharp) or "
               "bilinear (soft). Applies at the next start.");
    choice_row(S_SHARP, "Sharpness", "FSR 1's sharpening. Applies at the next start.");
    opt_section("Image");
    choice_row(S_AA, "Anti-aliasing", "Smooths the 3D scene's edges: the game's own FXAA, SMAA (sharper, a little slower) or "
               "none. Applies at the next start.");
    opt_end();
}

void page_performance(float rs) {
    opt_begin("##perf", rs);
    opt_section("Frame rate");
    choice_row(S_FPS, "Frame rate", "Frames a second: 30 is the game's own. Above 30 the game is stepped by the time its "
               "frames take; 60 when the console keeps up. Applies at the next start.");
    if (!fg_available()) opt_disable_next();
    static std::string fg_desc;
    const char *why = tsword_fg_status ? tsword_fg_status() : NULL;
    fg_desc = fg_available()
        ? "Frames made between the game's (Lossless Scaling's frame generation, with your own Lossless.dll): 2x, 3x or 4x "
          "the frames on the TV, the game's own a frame later. 3x and 4x want a 120 Hz TV. Applies at once."
        : "Frames made between the game's, with your own Lossless.dll (from Lossless Scaling, on Steam): copy it to the "
          "title folder's lsfg/Lossless.dll.";
    if (why) fg_desc += std::string(" Stopped: ") + why;
    choice_row(S_FG, "Frame generation", fg_desc.c_str());
    choice_row(S_SPEED, "Speed-ups", "Balanced: the game's graphics are translated and recorded on their own threads, as on "
               "the PC. Full: also shaders compiled and textures read ahead. Safe: everything on the game's own thread, "
               "as TSword ran while it froze after its first frame - slow; for when the others stop. Applies at the "
               "next start.");
    choice_row(S_PREEMPT, "Thread preemption", "The game's threads share its three cores: with this on, a more urgent "
               "thread takes a core at once instead of waiting for the one on it to stop (a long wait is a hitch: up "
               "to 125 ms in the 0.5.1 log). Experimental: TSword 0.4.2-0.4.5 froze with it on (other faults were "
               "found since). If the console freezes, set it off. Applies at the next start.");
    opt_section("Overlay");
    choice_row(S_OVERLAY, "Performance overlay", "Top right: the frame rate (FPS), or everything (Full) - the CPU, the "
               "temperatures, memory, the frame times and the resolution. Applies at once.");
    opt_end();
}

void page_controls(float rs) {
    opt_begin("##controls", rs);
    opt_section("Buttons");
    choice_row(S_LAYOUT, "Button layout", "Cross confirms, as on the PS5 (the game's A on Cross, B on Circle), or Circle "
               "confirms, as on a Switch. Applies at the next start.");
    opt_section("Touch pad");
    opt_action("Open or close this menu", "Two-finger tap", "Rest two fingers on the touch pad, without clicking it, and lift "
               "them. The game never reads the touch pad's touches, so nothing in it answers.");
    opt_action("Pages of the pause menu", "Swipe", "In the game's pause menu, a swipe left or right on the touch pad turns "
               "its pages (L and R).");
    opt_end();
}

void page_quit(float rs) {
    opt_begin("##quit", rs);
    opt_section("Quit");
    if (!g_quit_ask) {
        if (opt_button("Quit TSword", "Back to the PS5's home screen. What the game hasn't saved is lost: save first (the "
                       "game's pause menu, or wait for an autosave).")) g_quit_ask = true;
    } else {
        if (opt_button("Quit now", "TSword closes. What the game hasn't saved is lost.")) {
            LOG("quit from the menu\n");
            exit(0);
        }
        if (opt_button("Keep playing", "Back to the menu.")) g_quit_ask = false, opt_focus_first(true);
    }
    opt_end();
}

void c_settings(ImGuiIO &io, float s, float closing) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    double now = ImGui::GetTime();
    float k = c_ease((float)(now - g_view_at) / 0.38f), a = k * (1 - closing), dx = 18 * s * (1 - k);
    float W = io.DisplaySize.x, H = io.DisplaySize.y;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, c_fade(IM_COL32(10, 10, 11, 246), a));
    // L1 / R1: the categories
    int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ? 1 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ? -1 : 0;
    if (step) {
        g_ccat = (g_ccat + step + NCCATS) % NCCATS, g_quit_ask = false;
        opt_page_replay();
        opt_focus_first(true);
    }
    float mx = 64 * s + dx, top = 40 * s;
    float ts = 44 * s;
    c_text(dl, c_light(), ts, ImVec2(mx, top), c_fade(IM_COL32(242, 242, 243, 255), a), "Settings");
    c_text(dl, c_reg(), 20 * s, ImVec2(mx + c_width(c_light(), ts, "Settings") + 20 * s, top + ts * 0.45f),
           c_fade(IM_COL32(161, 161, 168, 255), a), k_ccats[g_ccat]);
    c_text(dl, c_med(), 15 * s, ImVec2(W - 64 * s - c_width(c_med(), 15 * s, "TSWORD"), top + ts * 0.45f),
           c_fade(IM_COL32(142, 142, 150, 255), a), "TSWORD");
    // the categories down the left: the open one white, the bar gliding to it
    float ry = top + 92 * s, rh = 56 * s, rw = 250 * s, cfs = 26 * s;
    float bar_target = ry + g_ccat * rh;
    g_cbar = g_cbar < 0 ? bar_target : c_approach(g_cbar, bar_target, 15);
    static float shift[NCCATS];
    for (int i = 0; i < NCCATS; i++) {
        float e = c_ease((float)(now - g_view_at - 0.04 - i * 0.03) / 0.34f) * (1 - closing);
        bool open_cat = i == g_ccat;
        shift[i] = c_approach(shift[i], open_cat ? 6 * s : 0);
        ImFont *f = open_cat ? c_med() : c_reg();
        c_text(dl, f, cfs, ImVec2(mx + 24 * s + shift[i], ry + i * rh + (rh - cfs) * 0.5f - 2 * s + 8 * s * (1 - e)),
               c_fade(open_cat ? IM_COL32(242, 242, 243, 255) : IM_COL32(124, 124, 132, 255), e), k_ccats[i]);
    }
    dl->AddRectFilled(ImVec2(mx, g_cbar + 10 * s), ImVec2(mx + 2.5f * s, g_cbar + rh - 10 * s), c_fade(IM_COL32(242, 242, 243, 255), a), 1.5f * s);
    // the page: the open category's rows, easing in
    float foot = 84 * s, px = mx + rw + 56 * s, pw = W - 64 * s - px, ph = H - ry - foot - 16 * s;
    ImGui::SetCursorScreenPos(ImVec2(px, ry - 6 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a < 0.02f ? 0.02f : a);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##cpage", ImVec2(pw, ph), ImGuiChildFlags_None, ImGuiWindowFlags_NavFlattened | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    float rs = ImGui::GetFontSize() / 34;
    ImGui::PushID(g_ccat);
    switch (g_ccat) {
    case 0: page_video(rs); break;
    case 1: page_performance(rs); break;
    case 2: page_controls(rs); break;
    case 3: page_quit(rs); break;
    }
    ImGui::PopID();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    // the foot: the selected row's description, the prompts
    float fy = H - foot - 8 * s;
    dl->AddLine(ImVec2(64 * s, fy), ImVec2(W - 64 * s, fy), c_fade(IM_COL32(30, 30, 33, 255), a), 1.2f * s);
    const char *hn, *hd;
    opt_console_help(&hn, &hd);
    if (g_help_name != hn) g_help_name = hn, g_help_at = now;
    float he = c_ease((float)(now - g_help_at) / 0.26f) * a;
    float hfs = 18 * s, room = W - 64 * s - 64 * s - 700 * s;
    dl->AddText(c_reg(), hfs, ImVec2(64 * s, fy + 14 * s + 4 * s * (1 - he)), c_fade(IM_COL32(161, 161, 168, 255), he), hd, NULL, room);
    static const CPrompt pr[] = {{-2, NULL, "Close"}, {1, NULL, "Back"}, {0, NULL, "Select"}, {-1, "L1 / R1", "Category"}};
    c_prompts(dl, W - 64 * s, fy + foot * 0.5f, s, pr, 4, a);
}

// The console menu's frame, while it is open; false once its sheet has sunk away (closed)
bool console_menu(ImGuiIO &io, float scale) {
    double now = ImGui::GetTime();
    if (!g_was_open) {   // opened: the quick menu rises, the cursor on Resume
        g_was_open = true, g_cview = CV_QUICK, g_open_at = g_view_at = now, g_close_at = -1;
        g_cfocus = 4, g_cfocus_on = 0, g_ring = ImVec4(-1, 0, 0, 0), g_cbar = -1, g_quit_ask = false;
        g_close_req = false;
    }
    if (g_close_req.exchange(false)) c_close();
    float closing = g_close_at < 0 ? 0 : (float)(now - g_close_at) / 0.22f;
    if (closing >= 1) {   // the sheet has sunk away: closed
        g_close_at = -1, g_was_open = false;
        return false;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 0.953f, 1));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.63f, 0.63f, 0.66f, 1));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.95f, 0.95f, 0.953f, 1));
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PushFont(c_reg());
    opt_console_look(c_reg(), c_med());
    int view = g_cview;
    bool asking = g_quit_ask;   // (before the page: Quit's Circle cancels its question this frame)
    if (view == CV_QUICK) c_quick(io, scale, closing);
    else c_settings(io, scale, closing);
    // Circle: the settings back to the quick menu (the Quit page's question first), the quick menu closes
    if (g_close_at < 0 && view == g_cview && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        if (view == CV_QUICK) c_close();
        else if (asking) g_quit_ask = false, opt_focus_first(true);
        else c_back_to_quick();
    }
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleColor(3);
    return true;
}
}  // namespace

// ------------------------------------------------------------------ ImGui on the present thread
namespace {
bool g_inited, g_failed;
VkFormat g_format;
PFN_vkGetInstanceProcAddr g_gipa;
PFN_vkCmdPipelineBarrier p_barrier;
PFN_vkCmdBeginRendering p_begin;
PFN_vkCmdEndRendering p_end;
PFN_vkCreateImageView p_create_view;
PFN_vkDestroyImageView p_destroy_view;
struct View { VkImage img; VkImageView view; };
View g_views[16];
int g_nviews;
double g_last_t;

PFN_vkVoidFunction load(const char *name, void *) {
    PFN_vkVoidFunction f = vk.vkGetDeviceProcAddr ? vk.vkGetDeviceProcAddr(vkc.device, name) : NULL;
    return f ? f : g_gipa(vkc.instance, name);
}

void check_vk(VkResult r) { if (r != VK_SUCCESS) LOG("ImGui's Vulkan backend: VkResult %d\n", (int)r); }

bool init(VkFormat fmt, VkExtent2D ext) {
    g_gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!g_gipa) return false;
    p_barrier = (PFN_vkCmdPipelineBarrier)load("vkCmdPipelineBarrier", NULL);
    p_begin = (PFN_vkCmdBeginRendering)load("vkCmdBeginRendering", NULL);
    p_end = (PFN_vkCmdEndRendering)load("vkCmdEndRendering", NULL);
    p_create_view = (PFN_vkCreateImageView)load("vkCreateImageView", NULL);
    p_destroy_view = (PFN_vkDestroyImageView)load("vkDestroyImageView", NULL);
    if (!p_barrier || !p_begin || !p_end || !p_create_view || !p_destroy_view) return false;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    io.DisplaySize = ImVec2((float)ext.width, (float)ext.height);
    load_fonts(ext.height / 1080.0f);
    style_init();
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, load, NULL)) return false;
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = vkc.instance;
    ii.PhysicalDevice = vkc.gpu;
    ii.Device = vkc.device;
    ii.QueueFamily = vkc.family;
    ii.Queue = vkc.queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = 8;   // (vertex buffers rotate per frame drawn: the present thread has 2 in flight)
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    g_format = fmt;
    ii.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_format;
    ii.CheckVkResultFn = check_vk;
    if (!ImGui_ImplVulkan_Init(&ii)) return false;
    // the font texture now, under the queue's lock (its upload submits on the queue the replayer shares)
    vk_queue_lock();
    bool ok = ImGui_ImplVulkan_CreateFontsTexture();
    vk_queue_unlock();
    if (!ok) return false;
    g_last_t = now_s();
    LOG("ImGui ready (%ux%u)\n", ext.width, ext.height);
    return true;
}

VkImageView view_for(VkImage img) {
    for (int i = 0; i < g_nviews; i++) if (g_views[i].img == img) return g_views[i].view;
    if (g_nviews == 16) return VK_NULL_HANDLE;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = g_format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v;
    if (p_create_view(vkc.device, &vi, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    g_views[g_nviews++] = {img, v};
    return v;
}

void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
             VkPipelineStageFlags s0, VkPipelineStageFlags s1) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src, b.dstAccessMask = dst, b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_barrier(cb, s0, s1, 0, 0, NULL, 0, NULL, 1, &b);
}

// the controller's state into ImGui's keys: what changed since the last frame, presses shorter than a frame included
void feed_input(ImGuiIO &io) {
    static uint32_t last;
    uint32_t held = g_held.load(), pressed = g_pressed.exchange(0);
    static const struct { int bit; ImGuiKey key; } map[] = {
        {0, ImGuiKey_GamepadFaceDown}, {1, ImGuiKey_GamepadFaceRight}, {2, ImGuiKey_GamepadFaceLeft},
        {3, ImGuiKey_GamepadFaceUp}, {4, ImGuiKey_GamepadBack}, {6, ImGuiKey_GamepadStart},
        {9, ImGuiKey_GamepadL1}, {10, ImGuiKey_GamepadR1}, {11, ImGuiKey_GamepadDpadUp}, {12, ImGuiKey_GamepadDpadDown},
        {13, ImGuiKey_GamepadDpadLeft}, {14, ImGuiKey_GamepadDpadRight}};   // (SDL_GamepadButton's numbers)
    for (auto &m : map) {
        bool now = held >> m.bit & 1, was = last >> m.bit & 1, tapped = pressed >> m.bit & 1;
        if (now != was) io.AddKeyEvent(m.key, now);
        else if (!now && tapped) io.AddKeyEvent(m.key, true), io.AddKeyEvent(m.key, false);   // (down and up between frames)
    }
    last = held;
    float lx = g_lx.load() / 32767.0f, ly = g_ly.load() / 32767.0f, dz = 0.35f;
    auto axis = [&](ImGuiKey k, float v) { io.AddKeyAnalogEvent(k, v > dz, v > dz ? (v - dz) / (1 - dz) : 0); };
    axis(ImGuiKey_GamepadLStickLeft, -lx), axis(ImGuiKey_GamepadLStickRight, lx);
    axis(ImGuiKey_GamepadLStickUp, -ly), axis(ImGuiKey_GamepadLStickDown, ly);
}
}  // namespace

// The menu is open or the overlay is shown: the present thread draws (and, while the menu is open, shows the last frame
// again every 33 ms so it moves while the game shows nothing new)
extern "C" int tsword_ui_animating(void) { return g_open.load(); }

// The swapchain is gone (present.c destroy_swapchain): its images' views go with it
extern "C" void tsword_ui_swapchain_gone(void) {
    for (int i = 0; i < g_nviews; i++) p_destroy_view(vkc.device, g_views[i].view, NULL);
    g_nviews = 0;
}

// present.c present_one, after the game's frame (and the text field) is in the swapchain image (TRANSFER_DST_OPTIMAL,
// left so): the overlay and the menu drawn over it. fresh: a new game frame (not the last one shown again); src: the
// game's picture's size.
extern "C" void tsword_ui_record(VkCommandBuffer cb, VkImage img, VkFormat fmt, VkExtent2D ext, int fresh, uint32_t src_w,
                                 uint32_t src_h) {
    static bool loaded;
    if (!loaded) loaded = true, settings_load();   // (the settings at once: frame generation reads them too)
    if (fresh) perf_frame();
    g_perf.src_w = src_w, g_perf.src_h = src_h, g_perf.out_w = ext.width, g_perf.out_h = ext.height;
    bool open = g_open.load();
    int overlay = g_overlay_mode.load();
    if (g_failed || (!open && !overlay)) {
        if (open) g_open = false;   // (no menu to show: the controller back to the game)
        return;
    }
    if (!g_inited) {   // ImGui once something is to be drawn: never, for a player who uses neither
        if (!init(fmt, ext)) {
            g_failed = true, g_open = false;
            LOG("could not start ImGui: no overlay, no menu\n");
            return;
        }
        g_inited = true;
    }
    double t = now_s();
    float dt = (float)(t - g_last_t);
    g_last_t = t;
    if (fmt != g_format) {   // (a swapchain of another format: ImGui's pipeline is for the first)
        static bool said;
        if (!said) said = true, LOG("the swapchain's format changed (%d -> %d): nothing drawn\n", (int)g_format, (int)fmt);
        return;
    }
    VkImageView view = view_for(img);
    if (!view) return;
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)ext.width, (float)ext.height);
    io.DeltaTime = dt > 0.0001f ? (dt < 0.25f ? dt : 0.25f) : 0.0001f;
    if (open) feed_input(io);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    float scale = ext.height / 1080.0f;
    if (overlay && !(open && g_cview == CV_SETTINGS)) perf_overlay_draw(io, scale, overlay);   // (the settings fill the screen)
    if (open && !console_menu(io, scale)) {
        g_open = false;   // (the controller back to the game: sdl_ps5.c keeps what is still held from it)
        LOG("menu closed\n");
    }
    ImGui::Render();
    ImDrawData *dd = ImGui::GetDrawData();
    if (!dd || dd->TotalVtxCount == 0) return;
    barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, ext};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    p_begin(cb, &ri);
    ImGui_ImplVulkan_RenderDrawData(dd, cb);
    p_end(cb);
    barrier(cb, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
}
