#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ct2::gfx {

// 64-byte aligned render-state snapshot diffed with ARM64 NEON (SSE2 fallback).
struct alignas(64) GfxState {
    uint32_t render_target_va = 0;
    uint32_t depth_target_va = 0;
    uint32_t shader_key = 0;
    uint32_t blend_state = 0;
    uint32_t depth_stencil_state = 0;
    uint32_t raster_state = 0;
    uint32_t topology = 0;
    uint32_t vertex_stride = 0;
    std::array<uint32_t, 8> tex_va{};
    std::array<uint32_t, 8> sampler_state{};
    std::array<float, 16> mvp{};
    std::array<float, 16> constants{};
    std::array<uint32_t, 8> viewport_scissor{};
    std::array<uint8_t, 64> extra_flags{};
};
static_assert(sizeof(GfxState) % 64 == 0, "GfxState size must be a multiple of 64 bytes");

struct StateRange {
    size_t offset;
    size_t size;
};

enum class CmdType : uint8_t {
    StatePatch,
    DrawIndexed,
    Clear,
    CopySurface,
    Present,
    FenceSync,
};

struct alignas(32) ResourceHeader {
    uint32_t guest_va;
    uint32_t byte_size;
    uint16_t width;
    uint16_t height;
    uint16_t format;
    uint16_t flags;
    uint32_t stride;
};
static_assert(sizeof(ResourceHeader) == 32, "ResourceHeader is 32-byte aligned");

struct RenderCmd {
    CmdType type = CmdType::DrawIndexed;
    uint32_t arg0 = 0;
    uint32_t arg1 = 0;
    uint32_t arg2 = 0;
    uint32_t header_ring_offset = 0;
    uint64_t fence_id = 0;
};

struct CmdBatch {
    std::vector<RenderCmd> commands;
    std::vector<uint8_t> state_deltas;
};

// Decouples title simulation from Vulkan replay:
//  - Title thread diffs active 64-byte state ranges via ARM64 NEON
//  - Batches up to kBatchCommands = 64 commands per submission
//  - Copies small 32-byte resource headers into a 1 MB ring buffer
//  - Dedicated render thread replays batches onto Vulkan asynchronously
class RenderThread {
public:
    static constexpr size_t kPiece = 64;
    static constexpr size_t kBatchCommands = 64;
    static constexpr size_t kHeaderRingBytes = 1u << 20; // 1 MB header ring

    using BatchReplayFn = std::function<void(const CmdBatch&, const uint8_t* header_ring)>;

    RenderThread() = default;
    ~RenderThread();

    void Start(BatchReplayFn replay_fn);
    void Stop();

    // Title-thread recording API
    uint32_t PushResourceHeader(const ResourceHeader& hdr);
    void EmitStateDiff(const GfxState& current);
    void EnqueueCommand(const RenderCmd& cmd);
    void FlushBatch();
    void WaitForFence(uint64_t fence_id);
    uint64_t SignalFence();

    // SIMD 64-byte chunk equality helper (ARM64 NEON / x86-64 SSE2)
    static bool SamePiece(const uint8_t* a, const uint8_t* b);
    static const std::vector<StateRange>& ActiveStateRanges();

private:
    void WorkerMain();

    BatchReplayFn replay_fn_;
    std::thread worker_;
    std::atomic<bool> running_{false};

    // Title-thread state mirror and pending batch
    GfxState last_sent_state_{};
    CmdBatch pending_batch_{};

    // 1 MB resource header ring buffer
    std::vector<uint8_t> header_ring_;
    size_t ring_cursor_ = 0;

    // Cross-thread batch queue & fence tracking
    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::condition_variable fence_cv_;
    std::deque<CmdBatch> queue_;
    uint64_t next_fence_ = 1;
    uint64_t completed_fence_ = 0;
};

} // namespace ct2::gfx
