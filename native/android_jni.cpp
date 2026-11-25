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
#include "native/dynamic_resolution.h"
#include "native/stats.h"
#include "native/vulkan.h"
#include "recomp/x86_context.h"

#include <atomic>
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
        return false;
    }
    g_rt.kernel.Initialize(&g_rt.rom, g_rt.xbe_info);
    g_rt.cpu_ctx.eip = g_rt.xbe_info.entry_point;

    if (g_rt.running.exchange(true)) return true;
    g_rt.title_thread = std::thread([] {
        g_rt.watchdog.RegisterThread("TitleThread", pthread_self(), &g_rt.cpu_ctx.eip);
        uint64_t frame_index = 0;

        while (g_rt.running.load(std::memory_order_acquire)) {
            if (g_rt.paused.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }

            const auto t0 = std::chrono::steady_clock::now();
            ct2::recomp::StepGuestFrame(g_rt.cpu_ctx, frame_index++);

            const bool skip_draw = g_rt.game_loop.TickFrameSkip();
            if (!skip_draw && g_rt.d3d) {
                g_rt.d3d->Clear(0xFF000000u);
                g_rt.d3d->Swap();
                g_rt.watchdog.NotifyPresent();

                uint32_t new_scale = 100;
                if (g_rt.dyn_res.OnPresent(/*in_menu_or_pause=*/false, &new_scale)) {
                    if (g_rt.window) {
                        g_rt.renderer.Resize(
                            static_cast<uint32_t>(ANativeWindow_getWidth(g_rt.window)),
                            static_cast<uint32_t>(ANativeWindow_getHeight(g_rt.window)),
                            new_scale);
                    }
                }
            }

            const double title_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
            ct2::diag::RecordTitleFrameMs(title_ms);
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

    ct2::kernel::InitMemory(/*expand_ram_128mb=*/true);
    g_rt.save_guard = std::make_unique<ct2::host::SaveGuard>(g_rt.files_dir + "/saves");
    ct2::diag::InitTelemetry(g_rt.files_dir, g_rt.rom_dir);
    g_rt.watchdog.Start(g_rt.files_dir + "/logs");

    ct2::vk::RendererConfig cfg{};
    cfg.cache_dir = g_rt.cache_dir + "/shadercache";
    cfg.enable_preload = true;
    g_rt.renderer.Initialize(cfg);

    g_rt.render_thread.Start([](const ct2::gfx::CmdBatch& batch, const uint8_t* ring) {
        g_rt.renderer.ReplayBatch(batch, ring);
    });
    g_rt.d3d = std::make_unique<ct2::d3d::D3D8Bridge>(&g_rt.render_thread);

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
        ANativeWindow_release(g_rt.window);
        g_rt.window = nullptr;
    }
    if (surface) {
        g_rt.window = ANativeWindow_fromSurface(env, surface);
        if (g_rt.window) {
            g_rt.renderer.Resize(
                static_cast<uint32_t>(ANativeWindow_getWidth(g_rt.window)),
                static_cast<uint32_t>(ANativeWindow_getHeight(g_rt.window)),
                g_rt.dyn_res.CurrentPercent());
        }
    }
    ClearJniException(env, "nativeSetSurface");
}

JNIEXPORT void JNICALL
Java_com_ct2recomp_android_NativeBridge_nativeSetBatteryTempC(JNIEnv*, jclass, jfloat temp_c) {
    ct2::diag::SetCachedBatteryTempC(static_cast<float>(temp_c));
}

JNIEXPORT void JNICALL
Java_com_ct2recomp_android_NativeBridge_nativeSetForeground(JNIEnv*, jclass, jboolean fg) {
    const bool is_fg = (fg == JNI_TRUE);
    g_rt.paused.store(!is_fg, std::memory_order_release);
    g_rt.watchdog.SetForeground(is_fg);
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
