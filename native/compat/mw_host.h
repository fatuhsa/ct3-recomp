#pragma once

// Minimal host-side shims for the portable Vulkan backend from XboxXBERecomp
// (native/ct3_gpu_backend.cpp). The Xbox host layers live in native/ct3_app_host.cpp;
// these are the small game/host namespaces that used to come from the
// MostWanted game repository. Everything here is header-only so no extra TU
// needs to be compiled or linked.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "native/ct3_sdk_bridge.h"

struct SDL_Window;

namespace game {

struct Options {
    bool night_mode = false;
    uint32_t texture_resolution = 16384;
    uint32_t reflections = 3;  // kReflectionsOff: CT3 uses its own cube captures
    bool road_reflections = true;
    bool bloom = true;
    bool shadows = true;
    bool fog = true;
    uint32_t screen_filter = 0;
    uint32_t anti_aliasing = 0;
    bool shader_preload = true;
};

inline const Options& Opt() {
    thread_local Options opts{};
    const auto& cfg = ct2::sdk_bridge::Config();
    opts.bloom = cfg.bloom.load(std::memory_order_relaxed);
    opts.shadows = cfg.shadows.load(std::memory_order_relaxed);
    opts.fog = cfg.fog.load(std::memory_order_relaxed);
    opts.screen_filter = cfg.screen_filter.load(std::memory_order_relaxed);
    opts.anti_aliasing = cfg.anti_aliasing.load(std::memory_order_relaxed);
    opts.shader_preload = cfg.shader_preload.load(std::memory_order_relaxed);
    return opts;
}

// The Android window is 2030x900 on the target device; use its real aspect
// for the game's HUD instead of assuming 16:9.
inline float Aspect() {
    int w = 0, h = 0;
    ct2::sdk_bridge::WindowPixels(w, h);
    if (w <= 0 || h <= 0) return 16.0f / 9.0f;
    return std::max(float(w) / float(h), 4.0f / 3.0f);
}

inline constexpr uint32_t kAaSsaa = 1;
inline constexpr uint32_t kAaFxaa = 2;

inline std::atomic<uint32_t> g_env_cube{0};
inline std::atomic<uint32_t> g_road_reflection{0};

}  // namespace game

namespace host {

inline uint32_t SurfaceEpoch() { return ct2::sdk_bridge::SurfaceEpoch(); }
// The SDL window handle is only an opaque token here: the Vulkan surface is
// created directly from ct2::sdk_bridge::GetAndroidWindow().
inline SDL_Window* Window() {
    return reinterpret_cast<SDL_Window*>(ct2::sdk_bridge::GetAndroidWindow());
}
inline void WindowPixels(int& w, int& h) { ct2::sdk_bridge::WindowPixels(w, h); }
inline bool Foreground() { return ct2::sdk_bridge::IsForeground(); }

// Defined by native/ct3_gpu_backend.cpp next to gpu::g_gpu_load.
float VulkanGpuLoad();

}  // namespace host

namespace texpacks {

struct Bound {
    uint32_t hash = 0;
    uint32_t width = 0, height = 0;
};

inline std::string PathOf(uint32_t) { return {}; }
inline uint64_t Generation() { return 0; }
inline void Snapshot(std::unordered_map<uint32_t, Bound>&, uint64_t&) {}

}  // namespace texpacks

namespace video {
inline bool Capturing() { return false; }
inline bool Stopping() { return false; }
inline bool Busy() { return false; }
inline bool PhotoBusy() { return false; }
inline bool PhotoWanted(float&) { return false; }
inline void Size(uint32_t& w, uint32_t& h) { w = 0; h = 0; }
inline int64_t Stamp() { return -1; }
inline void Frame(const uint8_t*, uint32_t, uint32_t, int64_t) {}
inline void Photo(const uint8_t*, uint32_t, uint32_t) {}
inline void Finish() {}
}  // namespace video

namespace ui {
inline bool ApplyGameFont() { return false; }
inline void NewFrame(float, float) {}
}  // namespace ui

namespace touch {
inline void Draw() {}
}  // namespace touch
