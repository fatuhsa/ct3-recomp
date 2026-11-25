#include "native/ct3_sdk_bridge.h"

#include "host/log.h"
#include "native/vulkan.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#ifdef CT3_ENABLE_XBE_SDK
#include "app/app.h"
#include "audio/audio.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/kernel.h"
#endif

namespace ct2::sdk_bridge {

namespace {
LiveBootMetrics g_metrics;
std::atomic<ct2::vk::VulkanRenderer*> g_renderer{nullptr};
std::mutex g_paths_mu;
std::string g_files_dir;
std::string g_cache_dir;
} // namespace

LiveBootMetrics& Metrics() {
    return g_metrics;
}

void SetRenderer(ct2::vk::VulkanRenderer* renderer) {
    g_renderer.store(renderer, std::memory_order_release);
}

ct2::vk::VulkanRenderer* GetRenderer() {
    return g_renderer.load(std::memory_order_acquire);
}

void SetHostPaths(const std::string& files_dir, const std::string& cache_dir) {
    std::lock_guard<std::mutex> lock(g_paths_mu);
    g_files_dir = files_dir;
    g_cache_dir = cache_dir;
}

std::string GetFilesDir() {
    std::lock_guard<std::mutex> lock(g_paths_mu);
    return g_files_dir;
}

} // namespace ct2::sdk_bridge

#ifdef CT3_ENABLE_XBE_SDK

namespace {

std::recursive_mutex g_ui_lock;
app::Settings g_settings{};
std::atomic<bool> g_audio_started{false};
std::thread g_audio_thread;

void Ct3OnLoaded() {
    // Ensure Xbox Utility Cache Drive Z:\ is mapped before Crazy Taxi 3 caches Media assets
    ::kernel::CreateSymbolicLink("\\??\\Z:", "\\Device\\Harddisk0\\Partition5");
    ct2::log::Info("CT3Boot", "Crazy Taxi 3 XBE image mapped into guest RAM (entry=0x000E0C34, Z: linked)");
}

void Ct3PadHook(int port, input::Pad& pad) {
    if (port == 0) {
        pad.connected = true;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// XboxXBERecomp app:: host implementation for Android
// ---------------------------------------------------------------------------
namespace app {

const GameInfo kGame = {
    .title = L"Crazy Taxi 3: High Roller",
    .short_title = L"Crazy Taxi 3",
    .name = L"CrazyTaxi3",
    .log = "ct3",
    .settings = "ct3_settings.toml",
    .splash = nullptr,
    .missing = L"Place Crazy Taxi 3 - High Roller (USA).iso in /sdcard/crazy taxi",
    .images = {{"default.xbe", "CrazyTaxi3"}},
    .options = {},
    .load = nullptr,
    .save = nullptr,
    .init = nullptr,
    .loaded = &Ct3OnLoaded,
    .window = nullptr,
    .message = nullptr,
    .ui = nullptr,
    .pad = &Ct3PadHook,
    .rumble = nullptr,
};

Settings& Get() { return g_settings; }
void Load() {}
void Save() {}
void LogSettings() {}
void LogSystem() {}
void ApplyDisplay() {}

Mode Desktop() {
    if (auto* r = ct2::sdk_bridge::GetRenderer(); r && r->SwapchainWidth() && r->SwapchainHeight()) {
        return Mode{r->SwapchainWidth(), r->SwapchainHeight()};
    }
    return Mode{1280, 720};
}

Mode Output() { return Desktop(); }

float RenderScale(uint32_t guest_height) {
    const Mode m = Output();
    if (guest_height == 0) return 1.0f;
    return (static_cast<float>(m.height) * (static_cast<float>(g_settings.render_percent) / 100.0f)) /
           static_cast<float>(guest_height);
}

bool Widescreen() {
    const Mode m = Output();
    return (m.height > 0) && (m.width * 2 >= m.height * 3);
}

std::filesystem::path ExeDir() {
    const std::string dir = ct2::sdk_bridge::GetFilesDir();
    return dir.empty() ? std::filesystem::path(".") : std::filesystem::path(dir);
}

bool FindGame(GameFiles&) { return false; }
std::string Sha1(const std::vector<uint8_t>&) { return {}; }
bool PrepareGameCode(const GameFiles&) { return true; }
int Instance() { return 0; }
void* Window() { return nullptr; }
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}
std::recursive_mutex& UiLock() { return g_ui_lock; }

void Pace() {
    using Clock = std::chrono::steady_clock;
    static auto next_frame = Clock::now();
    const uint32_t cap = g_settings.fps_cap;
    if (cap == 0) return;
    const auto step = std::chrono::microseconds(1000000 / (cap <= 1 ? 60 : cap));
    const auto now = Clock::now();
    if (now < next_frame) {
        std::this_thread::sleep_until(next_frame);
        next_frame += step;
    } else {
        next_frame = now + step;
    }
}

} // namespace app

// ---------------------------------------------------------------------------
// XboxXBERecomp audio:: host implementation (48 kHz mixer pacer; AAudio hookable)
// ---------------------------------------------------------------------------
namespace audio {

void StartOutput(std::function<void(float* out, uint32_t frames)> render) {
    if (g_audio_started.exchange(true)) return;
    g_audio_thread = std::thread([fn = std::move(render)] {
        constexpr uint32_t kFrames = 480; // 10 ms @ 48 kHz
        std::vector<float> mix(kFrames * 2, 0.0f);
        auto next = std::chrono::steady_clock::now();
        while (g_audio_started.load(std::memory_order_acquire)) {
            if (fn) fn(mix.data(), kFrames);
            next += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(next);
        }
    });
    g_audio_thread.detach();
}

} // namespace audio

// ---------------------------------------------------------------------------
// XboxXBERecomp input:: host implementation (port 0 connected for Android)
// ---------------------------------------------------------------------------
namespace input {

void Init() {}

Pad Poll(int port) {
    Pad p{};
    if (port == 0) p.connected = true;
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
}

} // namespace input

#endif // CT3_ENABLE_XBE_SDK
