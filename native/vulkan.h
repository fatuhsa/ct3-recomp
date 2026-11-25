#pragma once

#include "gfx/render_thread.h"
#include "native/vk_shader.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if __has_include(<vulkan/vulkan.h>)
#define CT2_HAS_VULKAN_HEADERS 1
#include <vulkan/vulkan.h>
#else
#define CT2_HAS_VULKAN_HEADERS 0
#endif

namespace ct2::vk {

constexpr uint32_t kQualcommVendorId = 0x5143u;
constexpr uint32_t kArmVendorId = 0x13B5u;

constexpr uint32_t kFramesInFlight = 3;
constexpr uint64_t kUploadRingSize = 32ull << 20;   // 32 MB per frame upload arena
constexpr uint32_t kSetsPerPool = 1024;
constexpr uint32_t kPipeCacheSaveInterval = 64;     // Flush VkPipelineCache every 64 new PSOs

// Non-BC GPU safe texture cache caps (DXT/BC -> BGRA expands VRAM 4-8x)
constexpr uint32_t kTexturesKept = 1024;
constexpr uint64_t kTextureBytesCap = 384ull << 20; // 384 MB hard byte cap
constexpr uint64_t kTextureIdleEvictFrames = 600;
constexpr uint64_t kTextureSweepInterval = 60;

// Sampled dirty-surface hashing (MemSig)
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

// Budgeted async HD texture replacement streaming
constexpr uint64_t kTexPackReadyCapBytes = 96ull << 20;   // 96 MB FIFO ready cache
constexpr uint64_t kTexPackUploadBudgetPerFrame = 24ull << 20; // 24 MB/frame upload cap
constexpr uint32_t kNonBcMaxTextureDim = 512;

struct RendererConfig {
    std::string cache_dir = "shadercache";
    void* native_window = nullptr; // ANativeWindow* on Android
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t resolution_scale_pct = 100;
    bool enable_preload = true;
};

struct GpuCaps {
    uint32_t vendor_id = 0;
    uint32_t api_version = 0;
    std::string device_name;
    bool is_adreno = false;
    bool is_mali = false;
    bool supports_bc = false;
    bool use_dynamic_rendering = false;       // Forced OFF on Adreno (0x5143)
    bool use_timeline_semaphores = false;     // Strictly Vulkan 1.2+ core (never VK 1.1 ext)
    bool use_extended_dynamic_state = false;  // Dynamic topology & vertex strides
    float timestamp_period_ns = 1.0f;
};

struct CachedTexture {
    uint32_t guest_va = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t vram_bytes = 0;
    uint64_t last_epoch = 0;
    uint64_t last_sig = 0;
    uint64_t last_used_frame = 0;
    bool had_cpu_readback = false;
    std::vector<uint8_t> readback_copy; // Row-by-row memcmp after Sync to catch tiny HUD writes
#if CT2_HAS_VULKAN_HEADERS
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
#endif
};

struct PendingClear {
    bool active = false;
    float color[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float depth = 1.0f;
    uint32_t stencil = 0;
};

struct DecodedTexPack {
    uint64_t tex_hash = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba_pixels;
};

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();

    bool Initialize(const RendererConfig& cfg);
    void Shutdown();

    void Resize(uint32_t width, uint32_t height, uint32_t scale_pct);
    void ReplayBatch(const gfx::CmdBatch& batch, const uint8_t* header_ring);

    // Sampled guest surface hashing (<=64 KB whole, >64 KB 64x64B evenly spaced slices).
    static uint64_t MemSig(uint32_t guest_va, uint32_t total_bytes);

    // Async HD texture pack submission (decoded on background TexPacks thread).
    void QueueTexPackDecode(uint64_t tex_hash, const std::string& file_path);

    const GpuCaps& Caps() const { return caps_; }
    uint64_t TotalTextureBytes() const { return total_texture_bytes_; }

private:
    struct FrameSlot {
        uint64_t timeline_value = 0;
        bool pool_clean = true;
        bool pools_dirty = false;
        uint64_t set_gen = 0;
        uint64_t upload_used = 0;
        std::unordered_map<uint64_t, uint64_t> descriptor_cache; // key -> handle/slot
#if CT2_HAS_VULKAN_HEADERS
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkQueryPool query_pool = VK_NULL_HANDLE;
        VkBuffer upload_buf = VK_NULL_HANDLE;
        VkDeviceMemory upload_mem = VK_NULL_HANDLE;
        void* upload_mapped = nullptr;
        std::vector<VkImage> retired_images;
        std::vector<VkBuffer> retired_buffers;
        std::vector<VkImageView> retired_views;
#endif
    };

    bool InitVulkanDevice();
    void SelectMobileQuirks(uint32_t vendor_id, uint32_t api_version,
                            bool has_dyn_render_ext, bool has_ext_dyn_state);

    // Adreno async VkCommandPool reset worker (timeline semaphore gated).
    void StartPoolResetWorker();
    void StopPoolResetWorker();
    void QueuePoolReset(uint32_t frame_idx);
    void AwaitPoolReset(uint32_t frame_idx);

    // Non-BC safe texture cache eviction (count AND byte cap).
    void ReleaseOldTextures();
    bool CheckTextureDirty(CachedTexture& tex, uint32_t byte_size);

    // Cross-frame VkDescriptorSet caching + view_gen_ invalidation.
    uint64_t AcquireDescriptorSet(FrameSlot& frame, const gfx::GfxState& state);
    void RetireImageView();

    // Triple-buffered bump-pointer upload ring (16B/256B UBO alignment).
    uint64_t AllocateUpload(FrameSlot& frame, uint64_t bytes, uint64_t align);

    // Periodic VkPipelineCache persistence (every 64 new pipelines) & background preloading.
    void LoadPipelineCacheFromDisk();
    void SavePipelineCacheAtomic();
    void StartPreloadWorker();

    // Budgeted async texture replacement streaming (96 MB ready cap, 24 MB/frame upload).
    void StartTexPackWorker();
    void StopTexPackWorker();
    void ProcessTexPackUploadsForFrame();

    void ExecutePresent();

    RendererConfig cfg_{};
    GpuCaps caps_{};
    std::unique_ptr<ShaderCache> shader_cache_;

    gfx::GfxState active_state_{};
    PendingClear pending_clear_{};
    std::array<FrameSlot, kFramesInFlight> frames_{};
    uint32_t current_frame_ = 0;
    uint64_t present_count_ = 0;
    uint64_t timeline_counter_ = 0;
    uint64_t view_gen_ = 1;
    uint32_t new_pipelines_since_save_ = 0;

    std::unordered_map<uint64_t, CachedTexture> textures_;
    uint64_t total_texture_bytes_ = 0;

    // Async VkCommandPool reset thread state
    std::mutex reset_mu_;
    std::condition_variable reset_cv_;
    std::condition_variable reset_done_cv_;
    std::deque<uint32_t> reset_queue_;
    std::atomic<bool> reset_running_{false};
    std::thread reset_worker_;

    // Async TexPacks worker state
    std::mutex texpack_mu_;
    std::condition_variable texpack_cv_;
    std::deque<std::pair<uint64_t, std::string>> texpack_requests_;
    std::deque<DecodedTexPack> texpack_ready_;
    uint64_t texpack_ready_bytes_ = 0;
    std::atomic<bool> texpack_running_{false};
    std::thread texpack_worker_;

    std::thread preload_worker_;
    bool initialized_ = false;

#if CT2_HAS_VULKAN_HEADERS
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_dev_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    VkSemaphore timeline_sem_ = VK_NULL_HANDLE;
#endif
};

} // namespace ct2::vk
