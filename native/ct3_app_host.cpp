#include "native/ct3_sdk_bridge.h"

#include "host/log.h"
#include "native/vulkan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#endif

#ifdef CT3_ENABLE_XBE_SDK
#include "app/app.h"
#include "audio/audio.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/exports.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"
#endif

namespace ct2::sdk_bridge {

namespace {
LiveBootMetrics g_metrics;
std::atomic<ct2::vk::VulkanRenderer*> g_renderer{nullptr};

std::mutex g_paths_mu;
std::string g_files_dir;
std::string g_cache_dir;
std::string g_gpu_name;

std::mutex g_win_mu;
ANativeWindow* g_window = nullptr;
std::atomic<uint32_t> g_surface_epoch{1};
std::atomic<int> g_win_w{0};
std::atomic<int> g_win_h{0};
std::atomic<bool> g_foreground{true};

std::mutex g_pad_mu;
GamepadState g_touch_pad{};
GamepadState g_hw_pad{};
} // namespace

LiveBootMetrics& Metrics() {
    return g_metrics;
}

void SetGpuDeviceName(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_paths_mu);
    g_gpu_name = name;
}

std::string GetGpuDeviceName() {
    std::lock_guard<std::mutex> lock(g_paths_mu);
    return g_gpu_name;
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

std::string GetCacheDir() {
    std::lock_guard<std::mutex> lock(g_paths_mu);
    return g_cache_dir;
}

void SetAndroidWindow(ANativeWindow* window, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(g_win_mu);
    if (g_window != window) {
        g_window = window;
        g_surface_epoch.fetch_add(1, std::memory_order_acq_rel);
    }
    if (width > 0 && height > 0) {
        g_win_w.store(static_cast<int>(width), std::memory_order_release);
        g_win_h.store(static_cast<int>(height), std::memory_order_release);
    }
}

ANativeWindow* GetAndroidWindow() {
    std::lock_guard<std::mutex> lock(g_win_mu);
    return g_window;
}

uint32_t SurfaceEpoch() {
    return g_surface_epoch.load(std::memory_order_acquire);
}

void WindowPixels(int& w, int& h) {
    w = g_win_w.load(std::memory_order_acquire);
    h = g_win_h.load(std::memory_order_acquire);
}

void SetForeground(bool fg) {
    g_foreground.store(fg, std::memory_order_release);
}

bool IsForeground() {
    return g_foreground.load(std::memory_order_acquire);
}

void SetTouchPadState(const GamepadState& state) {
    std::lock_guard<std::mutex> lock(g_pad_mu);
    g_touch_pad = state;
}

void SetHardwarePadState(const GamepadState& state) {
    std::lock_guard<std::mutex> lock(g_pad_mu);
    g_hw_pad = state;
}

GamepadState GetMergedPadState() {
    std::lock_guard<std::mutex> lock(g_pad_mu);
    GamepadState m{};
    m.buttons = g_touch_pad.buttons | g_hw_pad.buttons;
    for (int i = 0; i < 8; ++i) {
        m.analog[i] = std::max(g_touch_pad.analog[i], g_hw_pad.analog[i]);
    }
    auto pick_axis = [](int16_t a, int16_t b) -> int16_t {
        return (std::abs(int(a)) >= std::abs(int(b))) ? a : b;
    };
    m.lx = pick_axis(g_touch_pad.lx, g_hw_pad.lx);
    m.ly = pick_axis(g_touch_pad.ly, g_hw_pad.ly);
    m.rx = pick_axis(g_touch_pad.rx, g_hw_pad.rx);
    m.ry = pick_axis(g_touch_pad.ry, g_hw_pad.ry);
    return m;
}

} // namespace ct2::sdk_bridge

#ifdef CT3_ENABLE_XBE_SDK

// ---------------------------------------------------------------------------
// Missing Xbox Kernel Ordinal 232 (NtUserIoApcDispatcher) for ReadFileEx APCs
// ---------------------------------------------------------------------------
KERNEL_EXPORT(232, NtUserIoApcDispatcher, 3) {
    // a[0] = ApcContext (LPOVERLAPPED_COMPLETION_ROUTINE)
    // a[1] = IoStatusBlock (also LPOVERLAPPED: +0 Status, +4 Information/BytesTransferred)
    const uint32_t routine = a[0];
    const uint32_t iosb = a[1];
    if (routine && iosb) {
        const uint32_t nt_status = ::kernel::R32(iosb + 0);
        const uint32_t bytes = ::kernel::R32(iosb + 4);
        const uint32_t win_err = (nt_status & 0x80000000u) ? nt_status : 0u;
        ::kernel::CallGuest(ctx, routine, {win_err, bytes, iosb});
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Additional XDK 4531 IDirectSoundBuffer methods called by Crazy Taxi 3
// (Prevents null dereference on host-allocated DirectSound Voice handles)
// ---------------------------------------------------------------------------
HLE_STDCALL(IDirectSoundBuffer_SetPitch, 2) { return 0; }
HLE_STDCALL(IDirectSoundBuffer_SetConeAngles, 4) { return 0; }
HLE_STDCALL(IDirectSoundBuffer_SetConeOrientation, 5) { return 0; }
HLE_STDCALL(IDirectSoundBuffer_SetConeOutsideVolume, 3) { return 0; }
HLE_STDCALL(IDirectSoundBuffer_SetMode, 3) { return 0; }
HLE_STDCALL(IDirectSoundBuffer_SetDistanceFactor, 3) { return 0; }

namespace {

std::recursive_mutex g_ui_lock;
app::Settings g_settings{};
std::atomic<bool> g_audio_started{false};
std::thread g_audio_thread;

void Ct3OnLoaded() {
    // The 4 GB guest reservation is PROT_NONE until mem::Commit is called, but
    // Crazy Taxi 3 touches user pages it never explicitly committed. The first
    // device tombstone showed a write to guest VA 0x06D92100; after committing
    // 0x00010000..0x7FFE0000 the next tombstone showed a NULL pointer deref at
    // guest VA 0x00000000 in sub_000F4D20 (a VM opcode reading [x1, x9] with
    // x9 == 0). Commit from VA 0 as well so the zero page behaves like a
    // zero-filled null object instead of SIGSEGV/SEGV_ACCERR. Physical pages are
    // still demand-allocated, so this costs no RAM up front.
    mem::Commit(0, mem::kUserHigh);
    ct2::log::Info("CT3Boot", "guest user VA 0x00000000..0x%08X committed for lazy touches", mem::kUserHigh);

    // Map Xbox Utility Cache Drive Z:\ before Crazy Taxi 3 caches Media assets
    ::kernel::CreateSymbolicLink("\\??\\Z:", "\\Device\\Harddisk0\\Partition5");
    // U: is the console's user save drive. Map it straight to the data partition
    // (E:): the canonical UDATA/TDATA split made Crazy Taxi 3 crash on a real
    // Mali-G57 device right after D3D init, so keep the mapping that was verified
    // to boot cleanly on-device. T: is deliberately left unmapped for the same
    // reason.
    ::kernel::CreateSymbolicLink("\\??\\U:", "\\Device\\Harddisk0\\Partition1");
    ct2::log::Info("CT3Boot", "Crazy Taxi 3 XBE mapped (entry=0x000E0C34, Z: and U: linked)");
}

void Ct3PadHook(int port, input::Pad& pad) {
    if (port != 0) return;
    pad.connected = true;
    const auto st = ct2::sdk_bridge::GetMergedPadState();
    pad.buttons |= st.buttons;
    for (int i = 0; i < 8; ++i) {
        pad.analog[i] = std::max(pad.analog[i], st.analog[i]);
    }
    if (st.lx != 0) pad.lx = st.lx;
    if (st.ly != 0) pad.ly = st.ly;
    if (st.rx != 0) pad.rx = st.rx;
    if (st.ry != 0) pad.ry = st.ry;
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
    int w = 0, h = 0;
    ct2::sdk_bridge::WindowPixels(w, h);
    if (w > 0 && h > 0) {
        return Mode{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    }
    return Mode{1280, 720};
}

Mode Output() { return Desktop(); }

float RenderScale(uint32_t guest_height) {
    const Mode m = Output();
    if (guest_height == 0) return 1.0f;
    // Cap internal render scale cleanly on Mali-G57 non-BC (1.25x = 800x600) for crisp 60 FPS
    const float raw = (static_cast<float>(m.height) * (static_cast<float>(g_settings.render_percent) / 100.0f)) /
                      static_cast<float>(guest_height);
    return std::clamp(raw, 1.0f, 1.25f);
}

bool Widescreen() {
    const Mode m = Output();
    return (m.height > 0) && (m.width * 2 >= m.height * 3);
}

std::filesystem::path ExeDir() {
    const std::string cache = ct2::sdk_bridge::GetCacheDir();
    if (!cache.empty()) return std::filesystem::path(cache);
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
// XboxXBERecomp audio:: host implementation (Android AAudio 48 kHz low-latency stereo)
// ---------------------------------------------------------------------------
namespace audio {

#ifdef __ANDROID__
namespace {
struct AAudioPump {
    std::function<void(float*, uint32_t)> render;
    AAudioStream* stream = nullptr;

    static aaudio_data_callback_result_t OnData(AAudioStream*, void* user, void* audio_data, int32_t num_frames) {
        auto* self = static_cast<AAudioPump*>(user);
        auto* out = static_cast<float*>(audio_data);
        if (!ct2::sdk_bridge::IsForeground() || !self->render || num_frames <= 0) {
            if (num_frames > 0) std::memset(out, 0, static_cast<size_t>(num_frames) * 2 * sizeof(float));
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        self->render(out, static_cast<uint32_t>(num_frames));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    static void OnError(AAudioStream*, void* user, aaudio_result_t err) {
        auto* self = static_cast<AAudioPump*>(user);
        ct2::log::Warn("CT3Audio", "AAudio stream disconnected/error (%d); reopening", int(err));
        std::thread([self] {
            self->Close();
            self->Open();
        }).detach();
    }

    bool Open() {
        AAudioStreamBuilder* b = nullptr;
        if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) return false;
        AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSampleRate(b, int32_t(kRate));
        AAudioStreamBuilder_setChannelCount(b, 2);
        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setDataCallback(b, &AAudioPump::OnData, this);
        AAudioStreamBuilder_setErrorCallback(b, &AAudioPump::OnError, this);
        const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream);
        AAudioStreamBuilder_delete(b);
        if (r != AAUDIO_OK || !stream) {
            stream = nullptr;
            return false;
        }
        AAudioStream_requestStart(stream);
        ct2::log::Info("CT3Audio", "AAudio 48 kHz low-latency stereo output active");
        return true;
    }

    void Close() {
        if (stream) {
            AAudioStream_requestStop(stream);
            AAudioStream_close(stream);
            stream = nullptr;
        }
    }
};
} // namespace
#endif

void StartOutput(std::function<void(float* out, uint32_t frames)> render) {
    if (g_audio_started.exchange(true)) return;
#ifdef __ANDROID__
    auto* pump = new AAudioPump{render, nullptr};
    if (pump->Open()) {
        return;
    }
    delete pump;
    ct2::log::Warn("CT3Audio", "AAudio unavailable; falling back to 10 ms software mixer pacer");
#endif
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
