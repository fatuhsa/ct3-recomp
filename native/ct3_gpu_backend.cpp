#include "native/ct3_sdk_bridge.h"

#include "host/log.h"
#include "native/stats.h"
#include "native/vulkan.h"

#ifdef CT3_ENABLE_XBE_SDK
#include "gpu/backend.h"

namespace gpu {

namespace {

// Bridges XboxXBERecomp's D3D8 gpu::Backend into our mobile VulkanRenderer
// (Mali-G57 non-BC 384 MB cap, VkSwapchainKHR present, VkQueryPool timings).
class Ct3VulkanBackend final : public Backend {
public:
    bool Init(uint32_t width, uint32_t height) override {
        auto& m = ct2::sdk_bridge::Metrics();
        m.d3d_width.store(width, std::memory_order_release);
        m.d3d_height.store(height, std::memory_order_release);
        ct2::log::Info("CT3Gpu", "XboxXBERecomp D3D8 Backend::Init(%ux%u)", width, height);
        return true;
    }

    void Clear(const State&, uint32_t, const uint8_t*, uint32_t flags, uint32_t color, float,
               uint32_t) override {
        auto& m = ct2::sdk_bridge::Metrics();
        m.clears.fetch_add(1, std::memory_order_relaxed);
        if (flags & kClearTarget) {
            m.last_clear_argb.store(color, std::memory_order_release);
            if (auto* r = ct2::sdk_bridge::GetRenderer()) {
                r->SetGameClearColor(color);
            }
        }
    }

    void Draw(const State&, uint32_t, uint32_t, uint32_t count, const uint8_t*) override {
        auto& m = ct2::sdk_bridge::Metrics();
        m.draws.fetch_add(1, std::memory_order_relaxed);
        m.last_draw_verts.store(count, std::memory_order_relaxed);

        ct2::diag::FrameCounters fc{};
        fc.draws = 1;
        fc.verts = count;
        ct2::diag::AddCounters(fc);
    }

    void Copy(const Surface&, const Surface&, uint32_t, uint32_t, uint32_t) override {
        ct2::sdk_bridge::Metrics().copies.fetch_add(1, std::memory_order_relaxed);
    }

    void Present(const Surface&) override {
        ct2::sdk_bridge::Metrics().presents.fetch_add(1, std::memory_order_relaxed);
        if (auto* r = ct2::sdk_bridge::GetRenderer()) {
            r->PresentNow();
        }
    }

    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t*) override {}
};

} // namespace

std::unique_ptr<Backend> CreateBackend() {
    return std::make_unique<Ct3VulkanBackend>();
}

std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner) {
    // Pass through cleanly on ARM64 (replaces SSE2-only sdk/runtime/gpu/render_thread.cpp)
    return inner;
}

} // namespace gpu

#endif // CT3_ENABLE_XBE_SDK
