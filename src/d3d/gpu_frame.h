#pragma once
#include <atomic>
#include <cstdint>

namespace gpu {
// Wait until the GPU has drawn present number n, counted from 0; any thread
void WaitGpuFrame(uint64_t n);

struct PerfCounters {
    std::atomic<uint64_t> render_ns{0};
    std::atomic<uint64_t> shader_ns{0}, shaders{0};
    std::atomic<uint64_t> pipe_ns{0}, pipes{0};
    std::atomic<uint64_t> tex_ns{0}, tex_bytes{0}, texes{0};
    std::atomic<uint64_t> stall_ns{0}, stalls{0};
    std::atomic<uint64_t> frame_wait_ns{0};
    std::atomic<uint64_t> acquire_ns{0};
    std::atomic<uint64_t> gpu_ns{0};
    std::atomic<uint64_t> draws{0}, submits{0};
    std::atomic<uint64_t> passes{0}, pass_px{0};
};
inline PerfCounters g_perf;

inline uint64_t g_vsc_changes = 0;
inline bool g_vsc_counted = false;
} // namespace gpu
