// (TSword PS5) The console menu's settings rows: see option_rows.h. Ported from WSword PS5's runtime/ui/option_rows.mm
// (its console look only: TSword has no mouse, no two-pane pages and no text rows).
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"  // (SetKeyOwner, NavCursorVisible, SetFocusID)

#include "option_rows.h"

static float g_scale = 1;
static std::string g_desc_focus, g_name_focus, g_desc_first, g_name_first;
static ImFont *g_con_reg, *g_con_med;
static std::string g_con_name, g_con_desc;  // the row the foot describes
static double g_page_t0;                    // when the page's rows began to ease in
static bool g_page_replay;
static int g_row_index;                     // the rows drawn so far on this page (their entrance is staggered)
static bool g_is_toggle, g_toggle_on;       // (the next row is opt_toggle's: a switch, and its state)
static bool g_next_disabled;
static bool g_focus_req, g_take_first;      // (opt_focus_first: requested / this page's first row takes it)

void opt_console_look(ImFont *regular, ImFont *medium) { g_con_reg = regular, g_con_med = medium; }
void opt_console_help(const char **name, const char **desc) { *name = g_con_name.c_str(), *desc = g_con_desc.c_str(); }
void opt_page_replay(void) { g_page_replay = true; }
void opt_disable_next(void) { g_next_disabled = true; }
void opt_focus_first(bool on) { g_focus_req = on; }

static float ease_out(float x) { x = x < 0 ? 0 : x > 1 ? 1 : x; float u = 1 - x; return 1 - u * u * u; }
static ImU32 fade(ImU32 c, float a) {
    int al = (int)((c >> IM_COL32_A_SHIFT & 0xff) * (a < 0 ? 0 : a > 1 ? 1 : a) + 0.5f);
    return (c & ~IM_COL32_A_MASK) | ((ImU32)al << IM_COL32_A_SHIFT);
}
static ImU32 mix(ImU32 a, ImU32 b, float t) {
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t,
                                                 x.w + (y.w - x.w) * t));
}
static float approach(float cur, float target) {  // a value moving smoothly toward its target (about 150 ms)
    float k = 1 - expf(-ImGui::GetIO().DeltaTime * 16);
    float n = cur + (target - cur) * k;
    return fabsf(n - target) < 0.002f ? target : n;
}
static float entrance(int i) {  // a row's entrance (index i on the page): how far in it is, 0..1
    float t = (float)(ImGui::GetTime() - g_page_t0) - (i < 12 ? i : 12) * 0.035f;
    return ease_out(t / 0.36f);
}
static const ImU32 kInk = IM_COL32(242, 242, 243, 255), kMuted = IM_COL32(161, 161, 168, 255),
                   kDim = IM_COL32(110, 110, 118, 255), kSel = IM_COL32(28, 28, 31, 255), kTrack = IM_COL32(44, 44, 49, 255);

void opt_begin(const char *id, float scale) {
    g_scale = scale;
    static std::string page;
    if (page != id || g_page_replay) {
        page = id, g_page_t0 = ImGui::GetTime(), g_page_replay = false;
        g_con_name.clear(), g_con_desc.clear();
    }
    g_row_index = 0;
    g_take_first = g_focus_req;
    g_focus_req = false;
    if (g_take_first) ImGui::NavMoveRequestCancel();
    g_desc_focus.clear(), g_name_focus.clear(), g_desc_first.clear(), g_name_first.clear();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##rows", avail, ImGuiChildFlags_None, ImGuiWindowFlags_NavFlattened | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
}

void opt_end(void) {
    ImGui::PopStyleVar();
    ImGui::EndChild();
    if (!g_name_focus.empty()) g_con_name = g_name_focus, g_con_desc = g_desc_focus;
    else if (g_con_name.empty()) g_con_name = g_name_first, g_con_desc = g_desc_first;
    ImGui::PopID();
}

// a section's title: small grey capitals, easing in with the rows
void opt_section(const char *title) {
    std::string t = title;
    for (char &c : t) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    float s = g_scale;
    if (ImGui::GetCursorPosY() > ImGui::GetStyle().WindowPadding.y + 1) ImGui::Dummy(ImVec2(0, 16 * s));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float fs = ImGui::GetFontSize() * 0.66f, w = ImGui::GetContentRegionAvail().x, e = entrance(g_row_index++);
    ImFont *f = g_con_reg ? g_con_reg : ImGui::GetFont();
    ImGui::GetWindowDrawList()->AddText(f, fs, ImVec2(p.x + 22 * s, p.y + 10 * s * (1 - e)), fade(IM_COL32(142, 142, 150, 255), e),
                                        t.c_str());
    ImGui::Dummy(ImVec2(w, fs + 10 * s));
}

// One row: draws it, handles Cross and left / right while selected. Returns 2 (Cross), -1 / +1 (left / right) or 0.
// arrows: a value that steps (‹ value › on the selected row). bar: a slider's position, 0..1 (-1: none).
static int row(const char *label, const std::string &value, bool changed, const char *desc, bool arrows = false, float bar = -1) {
    ImGui::PushID(label);
    const char *name = label;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float s = g_scale, fs = ImGui::GetFontSize();
    float w = ImGui::GetContentRegionAvail().x, h = fmaxf(fs * 2.05f, 58 * s);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the row's own frame shows the selection)
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    ImGuiID id = ImGui::GetItemID();
    if (g_take_first) {
        g_take_first = false;
        ImGui::SetFocusID(id, ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        ImGui::SetScrollHereY(0);
    }
    bool off = g_next_disabled;  // (greyed out: still selectable, so its description shows, but it can't change)
    g_next_disabled = false;
    bool foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible, sel = foc;
    int step = clicked && !off ? 2 : 0;
    if (foc && !off) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight)) step = 1;
        // left / right belong to the row (navigation would otherwise try to move sideways)
        for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight})
            ImGui::SetKeyOwner(k, id);
    }
    if (foc) g_desc_focus = desc ? desc : "", g_name_focus = name;
    if (g_name_first.empty()) g_desc_first = desc ? desc : "", g_name_first = name;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    bool toggle = g_is_toggle, on = g_toggle_on;
    g_is_toggle = false;
    ImGuiStorage *st = ImGui::GetStateStorage();
    float e = entrance(g_row_index++), dy = 10 * s * (1 - e);
    // (by value, never GetFloatRef: a new key can grow the storage and leave an earlier pointer into freed memory)
    int hsh = (int)ImHashStr(value.c_str());
    float sel_v = approach(st->GetFloat(id + 1, 0), sel ? 1.0f : 0.0f);
    float knob_v = approach(st->GetFloat(id + 2, on ? 1.0f : 0.0f), on ? 1.0f : 0.0f);
    float bar_v = st->GetFloat(id + 3, bar < 0 ? 0 : bar);
    if (bar >= 0) bar_v = approach(bar_v, bar);
    float change_v = st->GetFloat(id + 5, -10);
    if (st->GetInt(id + 4, hsh) != hsh) change_v = (float)ImGui::GetTime();
    st->SetFloat(id + 1, sel_v), st->SetFloat(id + 2, knob_v), st->SetFloat(id + 3, bar_v);
    st->SetFloat(id + 5, change_v), st->SetInt(id + 4, hsh);
    float a = e * (off ? 0.45f : 1.0f);
    ImVec2 r0(p.x + 2 * s, p.y + 2 * s + dy), r1(p.x + w - 2 * s, p.y + h - 2 * s + dy);
    if (sel_v > 0.01f) {  // the highlight: a lighter grey with a thin light edge, fading in and out
        dl->AddRectFilled(r0, r1, fade(kSel, sel_v * e), 10 * s);
        dl->AddRect(r0, r1, fade(kInk, sel_v * e * (off ? 0.5f : 1.0f)), 10 * s, 0, 1.5f * s);
    }
    float cy2 = p.y + h * 0.5f + dy, pad2 = 22 * s, right = p.x + w - pad2;
    ImFont *lf = sel && g_con_med ? g_con_med : g_con_reg ? g_con_reg : ImGui::GetFont();
    // the value's part, from the right: a switch, a slider and its value, a value between arrows, or an action
    float vleft = right;
    if (toggle) {
        float sw = 52 * s, sh = 30 * s, x0 = right - sw, k = knob_v;
        dl->AddRectFilled(ImVec2(x0, cy2 - sh * 0.5f), ImVec2(right, cy2 + sh * 0.5f), fade(mix(kTrack, kInk, k), a), sh * 0.5f);
        float kr = sh * 0.5f - 3 * s, kx = x0 + 3 * s + kr + (sw - 6 * s - 2 * kr) * k;
        dl->AddCircleFilled(ImVec2(kx, cy2), kr, fade(mix(IM_COL32(142, 142, 150, 255), IM_COL32(10, 10, 11, 255), k), a), 24);
        vleft = x0;
    } else if (!value.empty()) {
        float vfs = fs * 0.95f, vt = (float)ImGui::GetTime() - change_v, ve = ease_out(vt / 0.22f);
        ImFont *vf = g_con_reg ? g_con_reg : ImGui::GetFont();
        float vw = vf->CalcTextSizeA(vfs, FLT_MAX, 0, value.c_str()).x;
        ImU32 vcol = off ? kDim : changed ? kInk : kMuted;
        if (bar >= 0) {  // a slider: its value at the right, the track before it
            float tw = 104 * s, x1 = right - tw - 18 * s, x0 = x1 - 300 * s * fminf(1.0f, w / (1100 * s));
            dl->AddText(vf, vfs, ImVec2(right - vw, cy2 - vfs * 0.5f), fade(vcol, a), value.c_str());
            float fx = x0 + (x1 - x0) * bar_v;
            dl->AddRectFilled(ImVec2(x0, cy2 - 2 * s), ImVec2(x1, cy2 + 2 * s), fade(kTrack, a), 2 * s);
            dl->AddRectFilled(ImVec2(x0, cy2 - 2 * s), ImVec2(fx, cy2 + 2 * s), fade(kInk, a), 2 * s);
            dl->AddCircleFilled(ImVec2(fx, cy2), (5 + 3 * sel_v) * s, fade(kInk, a), 20);
            vleft = x0;
        } else if (arrows) {  // ‹ value ›: the arrows on the selected row; a new value rises into place
            float aw = 7 * s, gap = 16 * s, cx1 = right - aw;
            float vx = cx1 - gap - vw, cx0 = vx - gap - aw;
            ImU32 acol = fade(kInk, a * sel_v * (off ? 0 : 1));
            if (sel_v > 0.01f) {
                dl->AddLine(ImVec2(cx0 + aw, cy2 - aw), ImVec2(cx0, cy2), acol, 2 * s);
                dl->AddLine(ImVec2(cx0, cy2), ImVec2(cx0 + aw, cy2 + aw), acol, 2 * s);
                dl->AddLine(ImVec2(cx1, cy2 - aw), ImVec2(cx1 + aw, cy2), acol, 2 * s);
                dl->AddLine(ImVec2(cx1 + aw, cy2), ImVec2(cx1, cy2 + aw), acol, 2 * s);
            }
            dl->AddText(vf, vfs, ImVec2(vx, cy2 - vfs * 0.5f + 6 * s * (1 - ve)), fade(vcol, a * (0.25f + 0.75f * ve)), value.c_str());
            vleft = cx0;
        } else {  // an action: its value in an outlined pill
            float ph = 36 * s, pw = vw + 36 * s, x0 = right - pw;
            dl->AddRect(ImVec2(x0, cy2 - ph * 0.5f), ImVec2(right, cy2 + ph * 0.5f), fade(IM_COL32(70, 70, 78, 255), a), ph * 0.5f, 0, 1.2f * s);
            dl->AddText(vf, vfs, ImVec2(x0 + 18 * s, cy2 - vfs * 0.5f), fade(kInk, a), value.c_str());
            vleft = x0;
        }
    } else {  // a button: a chevron
        float cs = 6 * s, cx = right - cs;
        dl->AddLine(ImVec2(cx - cs, cy2 - cs), ImVec2(cx, cy2), fade(kMuted, a), 2 * s);
        dl->AddLine(ImVec2(cx, cy2), ImVec2(cx - cs, cy2 + cs), fade(kMuted, a), 2 * s);
        vleft = cx - 2 * cs;
    }
    // the label, cut short before the value's part
    float lx = p.x + pad2, room = vleft - lx - 20 * s;
    const char *end = name;
    while (*end) {
        const char *next = end + 1;
        while (((unsigned char)*next & 0xc0) == 0x80) ++next;
        if (lf->CalcTextSizeA(fs, FLT_MAX, 0, name, next).x > room) break;
        end = next;
    }
    dl->AddText(lf, fs, ImVec2(lx, cy2 - fs * 0.52f), fade(off ? kDim : kInk, e), name, end);
    if (*end) dl->AddText(lf, fs, ImVec2(lx + lf->CalcTextSizeA(fs, FLT_MAX, 0, name, end).x, cy2 - fs * 0.52f), fade(kInk, e), "...");
    ImGui::PopID();
    return step;
}

bool opt_toggle(const char *label, bool *v, bool def, const char *desc) {
    g_toggle_on = *v;
    g_is_toggle = true;
    if (!row(label, *v ? "On" : "Off", *v != def, desc)) return false;  // (any step flips it)
    *v = !*v;
    return true;
}

bool opt_choice(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap) {
    int cur = *v >= 0 && *v < n ? *v : 0;
    int step = row(label, names[cur], cur != def, desc, true);
    if (!step) return false;
    int nv = cur + (step == 2 ? 1 : step);
    nv = wrap ? (nv + n) % n : nv < 0 ? 0 : nv >= n ? n - 1 : nv;
    if (nv == cur) return false;
    *v = nv;
    return true;
}

bool opt_int(const char *label, int *v, int lo, int hi, int step, int def, const char *fmt, const char *desc, const char *lo_name) {
    char buf[48];
    if (lo_name && *v == lo) snprintf(buf, sizeof buf, "%s", lo_name);
    else snprintf(buf, sizeof buf, fmt, *v);
    float frac = hi > lo ? (float)(*v - lo) / (float)(hi - lo) : 0;
    int s = row(label, buf, *v != def, desc, true, frac);
    if (!s) return false;
    int n = *v + (s == 2 ? step : s * step);
    if (s == 2 && n > hi) n = lo;  // (Cross past the top wraps; left / right stop at the ends)
    n = n < lo ? lo : n > hi ? hi : n;
    if (n == *v) return false;
    *v = n;
    return true;
}

bool opt_button(const char *label, const char *desc) { return row(label, "", false, desc) == 2; }
bool opt_action(const char *label, const char *value, const char *desc) { return row(label, value ? value : "", false, desc) == 2; }
