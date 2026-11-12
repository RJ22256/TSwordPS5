// Stand-in for Eden's common/settings.h for the offline shader tool: only the values the shader recompiler reads,
// fixed to Eden's defaults (no resolution scaling, loop safety checks on, no bloom fix, no renderer debug).
#pragma once
#include "common/common_types.h"

namespace Settings {
template <typename T>
struct Setting {
    T value{};
    T GetValue() const { return value; }
    operator T() const { return value; }
};
struct ResolutionScalingInfo {
    u32 up_scale{1};
    u32 down_shift{0};
    f32 up_factor{1.0f};
    f32 down_factor{1.0f};
    bool active{};
    bool downscale{};
};
struct Values {
    ResolutionScalingInfo resolution_info{};
    Setting<bool> fix_bloom_effects{};
    Setting<bool> renderer_debug{};
    Setting<bool> disable_shader_loop_safety_checks{};
};
inline Values values{};
} // namespace Settings
