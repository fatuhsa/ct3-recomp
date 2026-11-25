#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace ct2::vk {

// Bump whenever HLSL specialization / shader lowering changes so stale SPIR-V is regenerated.
constexpr uint32_t kShaderCacheVersion = 3;

struct PsoKey {
    uint64_t vs_hash = 0;
    uint64_t ps_hash = 0;
    uint32_t render_pass_key = 0;
    uint32_t blend_state = 0;
    uint32_t depth_stencil_state = 0;
    uint32_t raster_state = 0;

    bool operator==(const PsoKey& o) const {
        return vs_hash == o.vs_hash && ps_hash == o.ps_hash &&
               render_pass_key == o.render_pass_key && blend_state == o.blend_state &&
               depth_stencil_state == o.depth_stencil_state && raster_state == o.raster_state;
    }
};

struct PsoKeyHash {
    size_t operator()(const PsoKey& k) const;
};

struct PreloadShaderRec {
    uint64_t shader_key = 0;
    uint64_t source_hash = 0;
};

struct PreloadManifest {
    std::vector<PreloadShaderRec> shaders;
    std::vector<PsoKey> psos;
};

// Shader specialization, atomic SPIR-V disk cache, and preload.bin recorder:
//  - Specialize() rewrites HLSL helper functions accepting Texture*/Sampler* parameters into
//    per-call-site copies referencing globals directly (fixes ARM Mali VUID-06924 garbage
//    when glslang runs with ENABLE_OPT=OFF).
//  - Stores SPIR-V atomically as <hash_hex>_<kShaderCacheVersion>.spv (.tmp -> rename).
//  - Records first-seen shader/PSO keys to shadercache/preload.bin for <1s warm boots.
class ShaderCache {
public:
    explicit ShaderCache(std::string cache_dir);

    // Rewrite HLSL helpers with opaque Texture/Sampler parameters before glslang compilation.
    static std::string Specialize(const std::string& hlsl_source);

    static uint64_t HashSource(const std::string& text);

    bool LoadSpirv(uint64_t source_hash, std::vector<uint32_t>& out_words) const;
    bool SaveSpirvAtomic(uint64_t source_hash, const std::vector<uint32_t>& words) const;

    void RecordShaderForPreload(uint64_t shader_key, uint64_t source_hash);
    void RecordPsoForPreload(const PsoKey& key);
    PreloadManifest LoadPreloadManifest() const;

    bool ClaimPsoBuild(const PsoKey& key);

    const std::string& CacheDir() const { return cache_dir_; }

private:
    std::string SpirvPath(uint64_t source_hash) const;
    std::string PreloadPath() const;

    std::string cache_dir_;
    mutable std::mutex preload_mu_;
    std::unordered_set<uint64_t> recorded_shaders_;
    std::unordered_set<PsoKey, PsoKeyHash> recorded_psos_;

    std::mutex pre_lock_;
    std::unordered_set<PsoKey, PsoKeyHash> pre_claimed_;
};

} // namespace ct2::vk

#ifdef CT3_ENABLE_XBE_SDK
// Bindings, one descriptor set: b0 and b1 at 0 and 1, t0 to t3 at 2 to 5, s0 to s3 at 6 to 9.
namespace vkshader {

constexpr uint32_t kBindingConstants = 0; // b0
constexpr uint32_t kBindingTextures = 2;  // t0
constexpr uint32_t kBindingSamplers = 6;  // s0
constexpr uint32_t kBindings = 10;

enum class Stage { Vertex, Pixel };

void Init(const std::filesystem::path& cache_dir);
bool Compile(const std::string& hlsl, const char* entry, Stage stage, uint64_t hash, std::vector<uint32_t>& spirv);
bool Cached(uint64_t hash, std::vector<uint32_t>& spirv);

} // namespace vkshader
#endif // CT3_ENABLE_XBE_SDK
