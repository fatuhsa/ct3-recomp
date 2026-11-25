#pragma once

#include <atomic>
#include <cstdint>
#include <string>

struct ANativeWindow;

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

    // Live 3D VulkanBackend swapchain & device metrics
    std::atomic<bool> vk_ready{false};
    std::atomic<uint32_t> vendor_id{0};
    std::atomic<uint32_t> api_version{0};
    std::atomic<bool> dynamic_rendering{false};
    std::atomic<bool> timeline_sem{false};
    std::atomic<bool> bc_supported{false};
    std::atomic<uint32_t> swap_width{0};
    std::atomic<uint32_t> swap_height{0};
    std::atomic<uint32_t> swap_images{0};
    std::atomic<float> fps{0.0f};
    std::atomic<float> gpu_load_pct{0.0f};
};

struct GamepadState {
    uint16_t buttons = 0;
    uint8_t analog[8] = {}; // A, B, X, Y, Black, White, LT, RT
    int16_t lx = 0;
    int16_t ly = 0;
    int16_t rx = 0;
    int16_t ry = 0;
};

LiveBootMetrics& Metrics();

void SetGpuDeviceName(const std::string& name);
std::string GetGpuDeviceName();

void SetRenderer(ct2::vk::VulkanRenderer* renderer);
ct2::vk::VulkanRenderer* GetRenderer();

void SetHostPaths(const std::string& files_dir, const std::string& cache_dir);
std::string GetFilesDir();
std::string GetCacheDir();

// Exclusive ANativeWindow* ownership for 3D VulkanBackend
void SetAndroidWindow(ANativeWindow* window, uint32_t width, uint32_t height);
ANativeWindow* GetAndroidWindow();
uint32_t SurfaceEpoch();
void WindowPixels(int& w, int& h);
void SetForeground(bool fg);
bool IsForeground();

// Touch + hardware controller state for Xbox port 0
void SetTouchPadState(const GamepadState& state);
void SetHardwarePadState(const GamepadState& state);
GamepadState GetMergedPadState();

} // namespace ct2::sdk_bridge
