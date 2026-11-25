#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace ct2::vk {
class VulkanRenderer;
}

namespace ct2::sdk_bridge {

struct LiveBootMetrics {
    std::atomic<bool> booted{false};
    std::atomic<uint32_t> d3d_width{0};
    std::atomic<uint32_t> d3d_height{0};
    std::atomic<uint64_t> clears{0};
    std::atomic<uint64_t> draws{0};
    std::atomic<uint64_t> copies{0};
    std::atomic<uint64_t> presents{0};
    std::atomic<uint32_t> last_clear_argb{0};
    std::atomic<uint32_t> last_draw_verts{0};
};

LiveBootMetrics& Metrics();

void SetRenderer(ct2::vk::VulkanRenderer* renderer);
ct2::vk::VulkanRenderer* GetRenderer();

void SetHostPaths(const std::string& files_dir, const std::string& cache_dir);
std::string GetFilesDir();

} // namespace ct2::sdk_bridge
