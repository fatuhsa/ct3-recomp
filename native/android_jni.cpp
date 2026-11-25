#include "d3d/d3d8_bridge.h"
#include "game/game_loop.h"
#include "gfx/render_thread.h"
#include "host/hang_watchdog.h"
#include "host/log.h"
#include "host/memory.h"
#include "host/rom_vfs.h"
#include "host/save_guard.h"
#include "host/xbe_loader.h"
#include "kernel/xbox_kernel.h"
#include "native/ct3_sdk_bridge.h"
#include "native/dynamic_resolution.h"
#include "native/stats.h"
#include "native/vulkan.h"
#include "recomp/x86_context.h"

#ifdef CT3_ENABLE_XBE_SDK
#include "core/log.h"
#include "kernel/disc.h"
#include "kernel/kernel.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <jni.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <android/native_window_jni.h>

namespace {

struct RuntimeState {
    std::mutex mu;
    ct2::host::RomVfs rom;
    ct2::host::XbeImageInfo xbe_info{};
    ct2::xbox::XboxKernel kernel;
    std::unique_ptr<ct2::host::SaveGuard> save_guard;
    ct2::host::HangWatchdog watchdog;
    ct2::vk::VulkanRenderer renderer;
    ct2::gfx::RenderThread render_thread;
    std::unique_ptr<ct2::d3d::D3D8Bridge> d3d;
    ct2::game::GameLoop game_loop;
    ct2::perf::DynamicResolution dyn_res;
    ct2::recomp::X86Context cpu_ctx{};

    ANativeWindow* window = nullptr;
    std::string files_dir;
    std::string cache_dir;
    std::string rom_dir;
    std::string rom_open_path;
    int rom_saf_fd = -1;
    std::string boot_status = "Waiting for ROM";

    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};
    std::thread title_thread;
};

RuntimeState g_rt;

bool ClearJniException(JNIEnv* env, const char* context) {
    if (env->ExceptionCheck()) {
        ct2::log::Warn("CT3Jni", "Cleared pending JNI exception in %s", context);
        env->ExceptionClear();
        return true;
    }
    return false;
}

std::string JStringToStd(JNIEnv* env, jstring js) {
    if (!js) return {};
    const char* chars = env->GetStringUTFChars(js, nullptr);
    if (!chars) {
        ClearJniException(env, "GetStringUTFChars");
        return {};
    }
    std::string out(chars);
    env->ReleaseStringUTFChars(js, chars);
    return out;
}

bool BootLoadedRomLocked() {
    if (!g_rt.rom.LoadDefaultXbe(g_rt.xbe_info)) {
        ct2::log::Error("CT3Jni", "Mounted ROM does not contain a valid Original Xbox default.xbe");
        g_rt.boot_status = "Invalid XBE";
        return false;
    }
    g_rt.kernel.Initialize(&g_rt.rom, g_rt.xbe_info);
    g_rt.cpu_ctx.eip = g_rt.xbe_info.entry_point;

#ifdef CT3_ENABLE_XBE_SDK
    // Boot real statically recompiled 128-partition Crazy Taxi 3 XBE via portable XboxXBERecomp SDK!
    if (!ct2::sdk_bridge::Metrics().booted.exchange(true)) {
        std::error_code ec;
        const std::string log_dir = g_rt.files_dir + "/logs";
        std::filesystem::create_directories(log_dir, ec);
        logx::Init(log_dir, "ct3", false);

        std::vector<uint8_t> xbe_bytes;
        if (!g_rt.rom.ReadXboxFile("default.xbe", xbe_bytes)) {
            g_rt.boot_status = "Failed reading default.xbe";
            ct2::sdk_bridge::Metrics().booted.store(false);
            return false;
        }

        // Mount XISO into XboxXBERecomp kernel::Disc (direct path or /proc/self/fd/<fd>)
        std::string disc_path = g_rt.rom_open_path;
        if (disc_path.empty() && g_rt.rom_saf_fd >= 0) {
            disc_path = "/proc/self/fd/" + std::to_string(g_rt.rom_saf_fd);
        }
        if (!disc_path.empty()) {
            auto disc = std::make_unique<::kernel::Disc>();
            std::string err;
            if (disc->Open(disc_path, err)) {
                ::kernel::MountDisc(std::move(disc));
                ct2::log::Info("CT3Boot", "Mounted kernel::Disc from %s", disc_path.c_str());
            } else {
                ct2::log::Warn("CT3Boot", "kernel::Disc::Open(%s) failed: %s",
                               disc_path.c_str(), err.c_str());
            }
        }

        const std::string game_dir = g_rt.rom_dir.empty() ? g_rt.files_dir : g_rt.rom_dir;
        const std::string data_dir = g_rt.files_dir + "/xbox_hdd";
        std::filesystem::create_directories(data_dir, ec);

        if (::kernel::Boot(std::move(xbe_bytes), "default.xbe", game_dir, data_dir)) {
            g_rt.boot_status = "LIVE 3D (128 XBE Partitions)";
            ct2::log::Info("CT3Boot", "XboxXBERecomp kernel::Boot started Crazy Taxi 3 entry 0x%08X!",
                           g_rt.xbe_info.entry_point);
        } else {
            g_rt.boot_status = "kernel::Boot failed (check files/logs/ct3.log)";
            ct2::log::Error("CT3Boot", "XboxXBERecomp kernel::Boot failed!");
            return false;
        }
    }
#else
    g_rt.boot_status = "Diag Mode (SDK disabled)";
#endif

    if (g_rt.running.exchange(true)) return true;
    g_rt.title_thread = std::thread([] {
        g_rt.watchdog.RegisterThread("HostPacer", pthread_self(), &g_rt.cpu_ctx.eip);
        uint64_t last_presents = 0;

        while (g_rt.running.load(std::memory_order_acquire)) {
            if (g_rt.paused.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }

#ifdef CT3_ENABLE_XBE_SDK
            const uint64_t cur_presents = ct2::sdk_bridge::Metrics().presents.load(std::memory_order_relaxed);
            if (cur_presents != last_presents) {
                last_presents = cur_presents;
                g_rt.watchdog.NotifyPresent();
            }
#else
            if (!g_rt.game_loop.TickFrameSkip() && g_rt.d3d) {
                g_rt.d3d->Clear(0xFF000000u);
                g_rt.d3d->Swap();
                g_rt.watchdog.NotifyPresent();
            }
            (void)last_presents;
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        g_rt.watchdog.UnregisterThread(pthread_self());
    });
    return true;
}

} // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_ct2recomp_android_NativeBridge_nativeInit(JNIEnv* env, jclass,
                                                   jstring j_files_dir,
                                                   jstring j_cache_dir) {
    std::lock_guard<std::mutex> lock(g_rt.mu);
    g_rt.files_dir = JStringToStd(env, j_files_dir);
    g_rt.cache_dir = JStringToStd(env, j_cache_dir);
    ct2::sdk_bridge::SetHostPaths(g_rt.files_dir, g_rt.cache_dir);

    ct2::kernel::InitMemory(/*expand_ram_128mb=*/true);
    g_rt.save_guard = std::make_unique<ct2::host::SaveGuard>(g_rt.files_dir + "/saves");
    ct2::diag::InitTelemetry(g_rt.files_dir, g_rt.rom_dir);
    g_rt.watchdog.Start(g_rt.files_dir + "/logs");

#ifndef CT3_ENABLE_XBE_SDK
    ct2::vk::RendererConfig cfg{};
    cfg.cache_dir = g_rt.cache_dir + "/shadercache";
    cfg.enable_preload = true;
    g_rt.renderer.Initialize(cfg);
    ct2::sdk_bridge::SetRenderer(&g_rt.renderer);

    g_rt.render_thread.Start([](const ct2::gfx::CmdBatch& batch, const uint8_t* ring) {
        g_rt.renderer.ReplayBatch(batch, ring);
    });
    g_rt.d3d = std::make_unique<ct2::d3d::D3D8Bridge>(&g_rt.render_thread);
#endif

    ClearJniException(env, "nativeInit");
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_ct2recomp_android_NativeBridge_nativeOpenRomPath(JNIEnv* env, jclass,
                                                          jstring j_rom_path) {
    std::lock_guard<std::mutex> lock(g_rt.mu);
    const std::string path = JStringToStd(env, j_rom_path);
    bool ok = path.empty() ? g_rt.rom.AutoDiscoverFromSdcard() : g_rt.rom.OpenPath(path);
    if (ok) {
        g_rt.rom_open_path = g_rt.rom.Info().image_path;
        g_rt.rom_dir = g_rt.rom.Info().root_dir;
        ct2::diag::InitTelemetry(g_rt.files_dir, g_rt.rom_dir);
        ok = BootLoadedRomLocked();
    }
    ClearJniException(env, "nativeOpenRomPath");
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_ct2recomp_android_NativeBridge_nativeOpenRomFd(JNIEnv* env, jclass,
                                                        jint fd,
                                                        jstring j_display_name) {
    std::lock_guard<std::mutex> lock(g_rt.mu);
    const std::string name = JStringToStd(env, j_display_name);
    g_rt.rom_open_path.clear();
    g_rt.rom_saf_fd = static_cast<int>(fd);
    bool ok = g_rt.rom.OpenFd(static_cast<int>(fd), name);
    if (ok) {
        ok = BootLoadedRomLocked();
    }
    ClearJniException(env, "nativeOpenRomFd");
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_ct2recomp_android_NativeBridge_nativeSetSurface(JNIEnv* env, jclass, jobject surface) {
    std::lock_guard<std::mutex> lock(g_rt.mu);
    if (g_rt.window) {
#ifdef CT3_ENABLE_XBE_SDK
        ct2::sdk_bridge::SetAndroidWindow(nullptr, 0, 0);
#else
        g_rt.renderer.SetNativeWindow(nullptr, 0, 0);
#endif
        ANativeWindow_release(g_rt.window);
        g_rt.window = nullptr;
    }
    if (surface) {
        g_rt.window = ANativeWindow_fromSurface(env, surface);
        if (g_rt.window) {
            const uint32_t w = static_cast<uint32_t>(ANativeWindow_getWidth(g_rt.window));
            const uint32_t h = static_cast<uint32_t>(ANativeWindow_getHeight(g_rt.window));
#ifdef CT3_ENABLE_XBE_SDK
            ct2::sdk_bridge::SetAndroidWindow(g_rt.window, w, h);
#else
            g_rt.renderer.SetNativeWindow(g_rt.window, w, h);
            g_rt.renderer.Resize(w, h, g_rt.dyn_res.CurrentPercent());
#endif
        }
    }
    ClearJniException(env, "nativeSetSurface");
}

JNIEXPORT jstring JNICALL
Java_com_ct2recomp_android_NativeBridge_nativeGetDiagStatus(JNIEnv* env, jclass) {
    const auto snap = ct2::diag::LatestSnapshot();
    const auto& m = ct2::sdk_bridge::Metrics();

#ifdef CT3_ENABLE_XBE_SDK
    const std::string gpu_name = ct2::sdk_bridge::GetGpuDeviceName();
    const uint32_t vendor_id = m.vendor_id.load();
    const uint32_t api_ver = m.api_version.load();
    const uint32_t api_maj = (api_ver >> 22) & 0x7Fu;
    const uint32_t api_min = (api_ver >> 12) & 0x3FFu;
    const char* vendor_tag = (vendor_id == 0x5143) ? "Adreno" : ((vendor_id == 0x13B5) ? "Mali" : "Mobile");

    char buf[1280];
    std::snprintf(
        buf, sizeof(buf),
        "CT3 XBE Recomp: %s\n"
        "GPU: %s (%s 0x%04X) | VK %u.%u | %.1f FPS (GPU %.0f%%)\n"
        "Quirks: dynRender=%s | timeline=%s | BC=%s\n"
        "Swapchain: %s (%ux%u, %u imgs) | D3D8: %ux%u\n"
        "Draws: %llu | Clears: %llu (0x%08X) | Copies: %llu | Swaps: %llu\n"
        "ROM: %s (%zu files) | Bat %.1fC",
        g_rt.boot_status.c_str(),
        gpu_name.empty() ? "Init..." : gpu_name.c_str(),
        vendor_tag, vendor_id, api_maj, api_min,
        static_cast<double>(m.fps.load()),
        static_cast<double>(m.gpu_load_pct.load()),
        m.dynamic_rendering.load() ? "ON" : "CachedRP",
        m.timeline_sem.load() ? "VK1.2" : "VkFence",
        m.bc_supported.load() ? "HW" : "CPU-Cap384M",
        m.vk_ready.load() ? "OK" : "WAIT",
        m.swap_width.load(), m.swap_height.load(), m.swap_images.load(),
        m.d3d_width.load(), m.d3d_height.load(),
        static_cast<unsigned long long>(m.draws.load()),
        static_cast<unsigned long long>(m.clears.load()),
        m.last_clear_argb.load(),
        static_cast<unsigned long long>(m.copies.load()),
        static_cast<unsigned long long>(m.presents.load()),
        g_rt.rom.IsOpen() ? g_rt.rom.Info().display_name.c_str() : "Not loaded",
        g_rt.rom.Xiso().FileCount(),
        snap.battery_temp_c);
#else
    const auto& caps = g_rt.renderer.Caps();
    const uint32_t api_maj = (caps.api_version >> 22) & 0x7Fu;
    const uint32_t api_min = (caps.api_version >> 12) & 0x3FFu;
    const char* vendor_tag = caps.is_adreno ? "Adreno" : (caps.is_mali ? "Mali" : "Other");
    char buf[1280];
    std::snprintf(
        buf, sizeof(buf),
        "CT3 XBE Recomp: %s\n"
        "GPU: %s (%s 0x%04X) | VK %u.%u\n"
        "Swapchain: %s (%ux%u, %u imgs) | Presents: %llu",
        g_rt.boot_status.c_str(),
        caps.device_name.empty() ? "Unknown" : caps.device_name.c_str(),
        vendor_tag, caps.vendor_id, api_maj, api_min,
        g_rt.renderer.SwapchainReady() ? "OK" : "WAIT",
        g_rt.renderer.SwapchainWidth(), g_rt.renderer.SwapchainHeight(),
        g_rt.renderer.SwapchainImageCount(),
        static_cast<unsigned long long>(g_rt.renderer.PresentCount()));
#endif

    jstring res = env->NewStringUTF(buf);
    ClearJniException(env, "nativeGetDiagStatus");
    return res;
}

JNIEXPORT void JNICALL
Java_com_ct2recomp_android_NativeBridge_nativeSetBatteryTempC(JNIEnv*, jclass, jfloat temp_c) {
    ct2::diag::SetCachedBatteryTempC(static_cast<float>(temp_c));
}

JNIEXPORT void JNICALL
Java_com_ct2recomp_android_NativeBridge_nativeSetForeground(JNIEnv*, jclass, jboolean fg) {
    const bool is_fg = (fg == JNI_TRUE);
    g_rt.paused.store(!is_fg, std::memory_order_release);
    ct2::sdk_bridge::SetForeground(is_fg);
    g_rt.watchdog.SetForeground(is_fg);
}

JNIEXPORT void JNICALL
Java_com_ct2recomp_android_NativeBridge_nativeSetPadState(
    JNIEnv*, jclass, jboolean is_hardware, jint buttons,
    jint lx, jint ly, jint rx, jint ry,
    jint a, jint b, jint x, jint y,
    jint black, jint white, jint lt, jint rt) {
    ct2::sdk_bridge::GamepadState st{};
    st.buttons = static_cast<uint16_t>(buttons & 0xFFFF);
    st.lx = static_cast<int16_t>(std::clamp(int(lx), -32768, 32767));
    st.ly = static_cast<int16_t>(std::clamp(int(ly), -32768, 32767));
    st.rx = static_cast<int16_t>(std::clamp(int(rx), -32768, 32767));
    st.ry = static_cast<int16_t>(std::clamp(int(ry), -32768, 32767));
    st.analog[0] = static_cast<uint8_t>(std::clamp(int(a), 0, 255));
    st.analog[1] = static_cast<uint8_t>(std::clamp(int(b), 0, 255));
    st.analog[2] = static_cast<uint8_t>(std::clamp(int(x), 0, 255));
    st.analog[3] = static_cast<uint8_t>(std::clamp(int(y), 0, 255));
    st.analog[4] = static_cast<uint8_t>(std::clamp(int(black), 0, 255));
    st.analog[5] = static_cast<uint8_t>(std::clamp(int(white), 0, 255));
    st.analog[6] = static_cast<uint8_t>(std::clamp(int(lt), 0, 255));
    st.analog[7] = static_cast<uint8_t>(std::clamp(int(rt), 0, 255));
    if (is_hardware == JNI_TRUE) {
        ct2::sdk_bridge::SetHardwarePadState(st);
    } else {
        ct2::sdk_bridge::SetTouchPadState(st);
    }
}

JNIEXPORT void JNICALL
Java_com_ct2recomp_android_NativeBridge_nativeSetGraphicsSwitches(
    JNIEnv*, jclass, jint reflection_cadence, jboolean road_reflections, jboolean particle_draw,
    jboolean bloom_glow, jboolean screen_post_fx, jboolean ultrawide, jboolean frame_skip) {
    std::lock_guard<std::mutex> lock(g_rt.mu);
    auto& sw = g_rt.game_loop.Switches();
    switch (reflection_cadence) {
        case 0: sw.env_cubemap = ct2::game::ReflectionCadence::Off; break;
        case 1: sw.env_cubemap = ct2::game::ReflectionCadence::Full; break;
        case 4: sw.env_cubemap = ct2::game::ReflectionCadence::Quarter; break;
        default: sw.env_cubemap = ct2::game::ReflectionCadence::Half; break;
    }
    sw.road_reflections = (road_reflections == JNI_TRUE);
    sw.particle_draw = (particle_draw == JNI_TRUE);
    sw.bloom_glow = (bloom_glow == JNI_TRUE);
    sw.screen_post_fx = (screen_post_fx == JNI_TRUE);
    sw.ultrawide_hor_plus = (ultrawide == JNI_TRUE);
    sw.frame_skip_60hz = (frame_skip == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_ct2recomp_android_NativeBridge_nativeShutdown(JNIEnv*, jclass) {
    g_rt.running.store(false, std::memory_order_release);
    if (g_rt.title_thread.joinable()) g_rt.title_thread.join();

    std::lock_guard<std::mutex> lock(g_rt.mu);
    ct2::sdk_bridge::SetAndroidWindow(nullptr, 0, 0);
    ct2::sdk_bridge::SetRenderer(nullptr);
    g_rt.d3d.reset();
    g_rt.render_thread.Stop();
    g_rt.renderer.Shutdown();
    g_rt.watchdog.Stop();
    g_rt.kernel.Shutdown();
    g_rt.rom.Close();
    if (g_rt.window) {
        ANativeWindow_release(g_rt.window);
        g_rt.window = nullptr;
    }
    ct2::kernel::ShutdownMemory();
}

} // extern "C"
