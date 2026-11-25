#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace ct2::vk {

// Bump whenever HLSL specialization / shader lowering changes so stale SPIR-V is regenerated.
constexpr uint32_t kShaderCacheVersion = 2;

struct PsoKey {
    uint64_t vs_hash = 0;
    uint64_t ps_hash = 0;
    uint32_t render_pass_key = 0;
    uint32_t blend_state = 0;
    uint32_t depth_stencil_state = 0;
    uint32_t raster_state = 0;
    // Note: topology & vertex stride are dynamic (VK_EXT_extended_dynamic_state / VK 1.3)
    // and intentionally excluded from PsoKey to drastically reduce pipeline permutations.

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

    // Append-only preload.bin recording (always active, even when boot preloading is toggled off).
    void RecordShaderForPreload(uint64_t shader_key, uint64_t source_hash);
    void RecordPsoForPreload(const PsoKey& key);
    PreloadManifest LoadPreloadManifest() const;

    // Coordination between render thread and detached preloader worker:
    // returns true if caller claimed ownership of building this PSO key.
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
