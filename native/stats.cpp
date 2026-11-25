#include "native/stats.h"

#include "host/log.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unistd.h>

namespace fs = std::filesystem;

namespace ct2::diag {

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_perf_enabled{false};
std::atomic<float> g_battery_temp_c{0.0f};

std::mutex g_stats_mu;
std::array<uint64_t, static_cast<size_t>(ScopeId::Count)> g_scope_ns{};
FrameCounters g_counters{};
double g_title_ms_accum = 0.0;
double g_render_ms_accum = 0.0;
double g_gpu_ms_accum = 0.0;
uint32_t g_frames_in_window = 0;
uint64_t g_total_presents = 0;
Clock::time_point g_window_start = Clock::now();
TelemetrySnapshot g_latest{};

struct PassAccum {
    double total_ms = 0.0;
    uint64_t count = 0;
};
std::unordered_map<std::string, PassAccum> g_pass_timings;

// Layered CPU load fallback state (/proc/stat -> /proc/self/stat)
uint64_t g_prev_proc_ticks = 0;
Clock::time_point g_prev_cpu_sample = Clock::now();
bool g_cpu_sample_init = false;

float SampleCpuLoadPct() {
    // 1. Try /proc/stat (often denied on Android 8+)
    {
        std::ifstream f("/proc/stat");
        std::string cpu;
        uint64_t user = 0, nice = 0, sys = 0, idle = 0;
        if (f && (f >> cpu >> user >> nice >> sys >> idle) && cpu == "cpu") {
            static uint64_t prev_busy = 0, prev_total = 0;
            const uint64_t busy = user + nice + sys;
            const uint64_t total = busy + idle;
            float pct = 0.0f;
            if (prev_total > 0 && total > prev_total) {
                pct = 100.0f * static_cast<float>(busy - prev_busy) /
                      static_cast<float>(total - prev_total);
            }
            prev_busy = busy;
            prev_total = total;
            return pct;
        }
    }

    // 2. Fallback to /proc/self/stat (safe with spaces in process name via strrchr(')'))
    std::ifstream self("/proc/self/stat");
    if (!self) return 0.0f;
    std::string line;
    std::getline(self, line);
    const char* rparen = std::strrchr(line.c_str(), ')');
    if (!rparen) return 0.0f;

    // After ") ", field 3 is state; fields 14 & 15 are utime & stime (11th & 12th after ')')
    const char* p = rparen + 1;
    uint64_t utime = 0, stime = 0;
    int field = 3;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        const char* end = p;
        while (*end && *end != ' ') ++end;
        if (field == 14) utime = std::strtoull(p, nullptr, 10);
        if (field == 15) {
            stime = std::strtoull(p, nullptr, 10);
            break;
        }
        p = end;
        ++field;
    }

    const auto now = Clock::now();
    const uint64_t ticks = utime + stime;
    float pct = 0.0f;
    if (g_cpu_sample_init) {
        const double wall_sec =
            std::chrono::duration<double>(now - g_prev_cpu_sample).count();
        const long clk_tck = ::sysconf(_SC_CLK_TCK);
        const long nproc = ::sysconf(_SC_NPROCESSORS_CONF);
        if (wall_sec > 0.01 && clk_tck > 0 && nproc > 0 && ticks >= g_prev_proc_ticks) {
            const double denom = wall_sec * static_cast<double>(clk_tck) * static_cast<double>(nproc);
            pct = static_cast<float>(100.0 * static_cast<double>(ticks - g_prev_proc_ticks) / denom);
        }
    }
    g_prev_proc_ticks = ticks;
    g_prev_cpu_sample = now;
    g_cpu_sample_init = true;
    return pct;
}

float SampleGpuLoadPct(double gpu_ms_in_window, double wall_ms_in_window) {
    // 1. Try Qualcomm Adreno sysfs busy percentage (may be blocked by SELinux)
    {
        std::ifstream f("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage");
        int pct = -1;
        if (f && (f >> pct) && pct >= 0) {
            return static_cast<float>(pct);
        }
    }
    // 2. Fallback to Vulkan VkQueryPool GPU busy ratio
    if (wall_ms_in_window > 1.0 && gpu_ms_in_window > 0.0) {
        return static_cast<float>(100.0 * gpu_ms_in_window / wall_ms_in_window);
    }
    return 0.0f;
}

} // namespace

void InitTelemetry(const std::string& app_files_dir, const std::string& rom_dir) {
    std::error_code ec;
    bool enable = false;
    if (!app_files_dir.empty() && fs::exists(fs::path(app_files_dir) / "perf", ec)) {
        enable = true;
    }
    if (!rom_dir.empty() && fs::exists(fs::path(rom_dir) / "perf", ec)) {
        enable = true;
    }
    if (fs::exists("/sdcard/crazy taxi/perf", ec)) {
        enable = true;
    }
    g_perf_enabled.store(enable, std::memory_order_release);
    log::Info("CT2Perf", "Perf telemetry %s (toggle via empty 'perf' marker file)",
              enable ? "ENABLED" : "disabled");
}

bool Perf() {
    return g_perf_enabled.load(std::memory_order_relaxed);
}

void SetPerfEnabled(bool enabled) {
    g_perf_enabled.store(enabled, std::memory_order_release);
}

void SetCachedBatteryTempC(float temp_c) {
    g_battery_temp_c.store(temp_c, std::memory_order_relaxed);
}

void AddScopeNs(ScopeId id, uint64_t ns) {
    std::lock_guard<std::mutex> lock(g_stats_mu);
    g_scope_ns[static_cast<size_t>(id)] += ns;
}

void AddCounters(const FrameCounters& delta) {
    std::lock_guard<std::mutex> lock(g_stats_mu);
    g_counters.draws += delta.draws;
    g_counters.verts += delta.verts;
    g_counters.upload_bytes += delta.upload_bytes;
    g_counters.upload_overflows += delta.upload_overflows;
    g_counters.sigs += delta.sigs;
    g_counters.sig_bytes += delta.sig_bytes;
    g_counters.texups += delta.texups;
    g_counters.psos += delta.psos;
    g_counters.shaders += delta.shaders;
    g_counters.render_passes += delta.render_passes;
}

void RecordTitleFrameMs(double ms) {
    std::lock_guard<std::mutex> lock(g_stats_mu);
    g_title_ms_accum += ms;
}

void RecordRenderFrameMs(double ms) {
    std::lock_guard<std::mutex> lock(g_stats_mu);
    g_render_ms_accum += ms;
}

void RecordGpuFrameMs(double ms) {
    std::lock_guard<std::mutex> lock(g_stats_mu);
    g_gpu_ms_accum += ms;
}

void RecordGpuPassMs(const char* pass_name, double ms) {
    if (!Perf() || !pass_name) return;
    std::lock_guard<std::mutex> lock(g_stats_mu);
    auto& p = g_pass_timings[pass_name];
    p.total_ms += ms;
    p.count += 1;
}

void OnFramePresented() {
    std::lock_guard<std::mutex> lock(g_stats_mu);
    ++g_frames_in_window;
    ++g_total_presents;

    const auto now = Clock::now();
    const double wall_ms =
        std::chrono::duration<double, std::milli>(now - g_window_start).count();

    if (wall_ms >= 1000.0) {
        const double inv_frames = g_frames_in_window ? (1.0 / g_frames_in_window) : 0.0;
        g_latest.fps = static_cast<float>(g_frames_in_window * 1000.0 / wall_ms);
        g_latest.title_ms = static_cast<float>(g_title_ms_accum * inv_frames);
        g_latest.render_ms = static_cast<float>(g_render_ms_accum * inv_frames);
        g_latest.gpu_ms = static_cast<float>(g_gpu_ms_accum * inv_frames);
        g_latest.cpu_load_pct = SampleCpuLoadPct();
        g_latest.gpu_load_pct = SampleGpuLoadPct(g_gpu_ms_accum, wall_ms);
        g_latest.battery_temp_c = g_battery_temp_c.load(std::memory_order_relaxed);

        if (Perf()) {
            auto ms = [](ScopeId s) {
                return static_cast<double>(g_scope_ns[static_cast<size_t>(s)]) * 1e-6;
            };
            log::Info("CT2Perf",
                      "fps=%.1f title=%.2fms render=%.2fms gpu=%.2fms cpu=%.0f%% gpuLoad=%.0f%% bat=%.1fC "
                      "draws=%llu passes=%llu upload=%lluKB psos=%llu | "
                      "draw=%.1f asm=%.1f bind=%.1f sig=%.1f texup=%.1f comp=%.1f clr=%.1f cpy=%.1f pres=%.1f wait=%.1f swap=%.1f sync=%.1f",
                      g_latest.fps, g_latest.title_ms, g_latest.render_ms, g_latest.gpu_ms,
                      g_latest.cpu_load_pct, g_latest.gpu_load_pct, g_latest.battery_temp_c,
                      static_cast<unsigned long long>(g_counters.draws),
                      static_cast<unsigned long long>(g_counters.render_passes),
                      static_cast<unsigned long long>(g_counters.upload_bytes >> 10),
                      static_cast<unsigned long long>(g_counters.psos),
                      ms(ScopeId::Draw), ms(ScopeId::Assemble), ms(ScopeId::Bind),
                      ms(ScopeId::Sig), ms(ScopeId::TexUp), ms(ScopeId::Compile),
                      ms(ScopeId::Clear), ms(ScopeId::Copy), ms(ScopeId::Present),
                      ms(ScopeId::Wait), ms(ScopeId::Swap), ms(ScopeId::Sync));
        }

        g_scope_ns.fill(0);
        g_counters = FrameCounters{};
        g_title_ms_accum = 0.0;
        g_render_ms_accum = 0.0;
        g_gpu_ms_accum = 0.0;
        g_frames_in_window = 0;
        g_window_start = now;
    }

    // Dump per-render-pass GPU timestamp breakdown every 600 frames to spot ping-pong pass storms
    if (Perf() && (g_total_presents % 600 == 0) && !g_pass_timings.empty()) {
        log::Info("CT2Perf", "=== Per-Pass GPU Timings (600-frame dump) ===");
        for (const auto& kv : g_pass_timings) {
            const double avg = kv.second.count ? (kv.second.total_ms / kv.second.count) : 0.0;
            log::Info("CT2Perf", "  pass %-24s count=%-6llu avg=%.3fms total=%.1fms",
                      kv.first.c_str(),
                      static_cast<unsigned long long>(kv.second.count),
                      avg, kv.second.total_ms);
        }
        g_pass_timings.clear();
    }
}

TelemetrySnapshot LatestSnapshot() {
    std::lock_guard<std::mutex> lock(g_stats_mu);
    return g_latest;
}

} // namespace ct2::diag
