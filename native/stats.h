#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace ct2::diag {

enum class ScopeId : uint8_t {
    Draw = 0,
    Assemble,
    Bind,
    Sig,
    TexUp,
    Compile,
    Clear,
    Copy,
    Present,
    Wait,
    Swap,
    Sync,
    Count,
};

struct FrameCounters {
    uint64_t draws = 0;
    uint64_t verts = 0;
    uint64_t upload_bytes = 0;
    uint64_t upload_overflows = 0;
    uint64_t sigs = 0;
    uint64_t sig_bytes = 0;
    uint64_t texups = 0;
    uint64_t psos = 0;
    uint64_t shaders = 0;
    uint64_t render_passes = 0;
};

struct TelemetrySnapshot {
    float fps = 0.0f;
    float title_ms = 0.0f;
    float render_ms = 0.0f;
    float gpu_ms = 0.0f;
    float cpu_load_pct = 0.0f;
    float gpu_load_pct = 0.0f;
    float battery_temp_c = 0.0f;
};

// Detects empty "perf" marker file in app files dir or /sdcard/crazy taxi/perf.
void InitTelemetry(const std::string& app_files_dir, const std::string& rom_dir = {});
bool Perf();
void SetPerfEnabled(bool enabled);

// Cached battery temperature pushed periodically from Android BatteryManager (no per-frame JNI).
void SetCachedBatteryTempC(float temp_c);

// Record frame/pass metrics.
void AddScopeNs(ScopeId id, uint64_t ns);
void AddCounters(const FrameCounters& delta);
void RecordTitleFrameMs(double ms);
void RecordRenderFrameMs(double ms);
void RecordGpuFrameMs(double ms);
void RecordGpuPassMs(const char* pass_name, double ms);

// Called once per presented frame; emits 1-line summary once/sec and pass table every 600 frames.
void OnFramePresented();
TelemetrySnapshot LatestSnapshot();

// Zero-overhead RAII scope profiler when Perf() is disabled.
class PerfScope {
public:
    explicit PerfScope(ScopeId id) : id_(id), active_(Perf()) {
        if (active_) start_ = Clock::now();
    }
    ~PerfScope() {
        if (active_) {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                Clock::now() - start_)
                                .count();
            if (ns > 0) AddScopeNs(id_, static_cast<uint64_t>(ns));
        }
    }

    PerfScope(const PerfScope&) = delete;
    PerfScope& operator=(const PerfScope&) = delete;

private:
    using Clock = std::chrono::steady_clock;
    ScopeId id_;
    bool active_;
    Clock::time_point start_{};
};

} // namespace ct2::diag
