// Offline translation of TOTK's Maxwell shaders to SPIR-V with Eden's shader recompiler (src/shader_recompiler),
// for comparing it with the Ryujinx translator on the same shaders and the same default answers.
// Modeled on mk8-recomp's offline_nvn_shader.cpp (GPL-3.0-or-later), which drives suyu's recompiler the same way.
//
// usage: eden_xlate <shaders.tsv> <raw dir> <out dir> <shard> <shard count>
//   raw dir: <key>.ctl (0x880-byte NVN control section) and <key>.bin (the code as the game hands it over)
//   writes <out dir>/<key>.spv and prints one line per shader: key, ok|fail, seconds, SPIR-V bytes, message
// Default answers, as MacNativeGames gives the Ryujinx translator for the default bind state: every texture 2D
// RGBA8 float, vertex inputs and colour outputs float, viewport transform on.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace {

std::vector<char> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::vector<char> b(static_cast<size_t>(f.tellg()));
    f.seekg(0);
    f.read(b.data(), static_cast<std::streamsize>(b.size()));
    return b;
}

u32 word(const std::vector<char>& b, size_t at) {
    if (at + 4 > b.size()) throw std::runtime_error("read out of bounds");
    u32 v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

class NvnEnvironment final : public Shader::Environment {
public:
    NvnEnvironment(const std::vector<char>& ctl, const std::vector<char>& bin, bool compute) : code(bin) {
        program_start = word(ctl, 0x708);
        program_size = word(ctl, 0x6f8);
        c1_start = word(ctl, 0x700);
        c1_size = word(ctl, 0x6fc);
        if (program_start > code.size() || program_size > code.size() - program_start) throw std::runtime_error("program extent");
        if (c1_start > code.size() || c1_size > code.size() - c1_start) throw std::runtime_error("c1 extent");
        start_address = program_start;
        is_proprietary_driver = true;
        if (compute) {
            stage = Shader::Stage::Compute;
        } else {
            std::memcpy(&sph, code.data() + program_start, sizeof sph);
            const u32 type = sph.common0.shader_type;
            stage = type == 1 ? Shader::Stage::VertexB : type == 5 ? Shader::Stage::Fragment
                  : type == 4 ? Shader::Stage::Geometry : type == 2 ? Shader::Stage::TessellationControl
                  : type == 3 ? Shader::Stage::TessellationEval : throw std::runtime_error("unknown shader type");
        }
    }
    u64 ReadInstruction(u32 address) override {
        if (address + 8 > code.size()) throw std::runtime_error("instruction read out of bounds");
        u64 v;
        std::memcpy(&v, code.data() + address, 8);
        return v;
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        if (index == 1 && offset + 4 <= c1_size) return word(code, c1_start + offset);
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return Shader::TexturePixelFormat::A8B8G8R8_UNORM; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 2; }   // NVN binds texture handles in c[2]
    u32 LocalMemorySize() const override { return stage == Shader::Stage::Compute ? 0x800 : static_cast<u32>(sph.LocalMemorySize()); }
    u32 SharedMemorySize() const override { return 0xc000; }   // Maxwell's maximum: the control's own value is not decoded yet
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}

    u32 program_start{}, program_size{}, c1_start{}, c1_size{};

private:
    const std::vector<char>& code;
};

Shader::Profile desktop_profile() {
    // A current desktop GPU (RTX 30-series class) through Vulkan 1.3, as Eden's renderer would report it.
    Shader::Profile p{};
    p.supported_spirv = 0x00010600;
    p.unified_descriptor_binding = true;
    p.support_descriptor_aliasing = true;
    p.support_int8 = p.support_uniform_and_storage_buffer_8bit = p.support_storage_buffer_8bit = true;
    p.support_int16 = p.support_uniform_and_storage_buffer_16bit = p.support_storage_buffer_16bit = true;
    p.support_int64 = true;
    p.support_float_controls = true;
    p.support_separate_denorm_behavior = p.support_separate_rounding_mode = true;
    p.support_fp16_denorm_preserve = p.support_fp32_denorm_preserve = true;
    p.support_fp16_denorm_flush = p.support_fp32_denorm_flush = true;
    p.support_fp16_signed_zero_nan_preserve = p.support_fp32_signed_zero_nan_preserve = p.support_fp64_signed_zero_nan_preserve = true;
    p.support_quad_shuffles = p.support_vote = p.support_shuffle_relative = true;
    p.support_viewport_index_layer_non_geometry = true;
    p.support_typeless_image_loads = true;
    p.support_demote_to_helper_invocation = true;
    p.support_int64_atomics = p.support_shared_int64_atomics = true;
    p.support_derivative_control = true;
    p.support_native_ndc = true;
    p.support_scaled_attributes = true;
    p.support_multi_viewport = true;
    p.support_geometry_streams = true;
    p.support_sampled_image_array_nonuniform_indexing = p.support_storage_image_array_nonuniform_indexing = true;
    p.support_uniform_texel_buffer_array_nonuniform_indexing = p.support_storage_texel_buffer_array_nonuniform_indexing = true;
    p.warp_size_potentially_larger_than_guest = false;
    p.need_declared_frag_colors = true;
    p.max_subgroup_size = 32;
    p.min_ssbo_alignment = 16;
    p.max_user_clip_distances = 8;
    return p;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "usage: eden_xlate <shaders.tsv> <raw dir> <out dir> <shard> <shard count>\n";
        return 2;
    }
    const std::string raw = argv[2], out = argv[3];
    const int shard = std::stoi(argv[4]), shards = std::stoi(argv[5]);
    std::ifstream table(argv[1]);
    std::string line;
    std::getline(table, line);   // header
    const Shader::Profile profile = desktop_profile();
    Shader::HostTranslateInfo host{};
    host.support_float64 = host.support_float16 = host.support_int64 = true;
    host.support_snorm_render_buffer = host.support_viewport_index_layer = host.support_conditional_barrier = true;
    host.min_ssbo_alignment = 16;
    host.ApplyDescriptorLimitPolicy();
    for (int row = 0; std::getline(table, line); ++row) {
        if (row % shards != shard) continue;
        std::istringstream fields(line);
        std::string key, stage;
        std::getline(fields, key, '\t');
        std::getline(fields, stage, '\t');
        const auto t0 = std::chrono::steady_clock::now();
        std::string result = "ok", message;
        size_t bytes = 0;
        try {
            const auto ctl = read_file(raw + "/" + key + ".ctl");
            const auto bin = read_file(raw + "/" + key + ".bin");
            const bool compute = stage == "cs";
            NvnEnvironment env(ctl, bin, compute);
            Shader::ObjectPool<Shader::IR::Inst> inst;
            Shader::ObjectPool<Shader::IR::Block> blocks;
            Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow;
            Shader::Maxwell::Flow::CFG cfg(env, flow, env.StartAddress() + (compute ? 0 : 0x50), compute);
            auto program = Shader::Maxwell::TranslateProgram(inst, blocks, env, cfg, host);
            Shader::RuntimeInfo runtime{};
            runtime.input_topology = Shader::InputTopology::Triangles;
            runtime.generic_input_types.fill(Shader::AttributeType::Float);
            runtime.color_output_types.fill(Shader::AttributeType::Float);
            runtime.previous_stage_stores.mask.set();
            if (!compute) Shader::Maxwell::ConvertLegacyToGeneric(program, runtime);
            Shader::Backend::Bindings bindings{};
            const auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
            std::ofstream o(out + "/" + key + ".spv", std::ios::binary);
            o.write(reinterpret_cast<const char*>(spv.data()), static_cast<std::streamsize>(spv.size() * 4));
            bytes = spv.size() * 4;
        } catch (const std::exception& e) {
            result = "fail";
            message = e.what();
        } catch (...) {
            result = "fail";
            message = "unknown exception";
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        for (char& c : message) if (c == '\t' || c == '\n') c = ' ';
        std::printf("%s\t%s\t%.4f\t%zu\t%.200s\n", key.c_str(), result.c_str(), s, bytes, message.c_str());
        std::fflush(stdout);
    }
    return 0;
}
