#include "gfx/render_thread.h"

#include "host/log.h"

#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace ct2::gfx {

RenderThread::~RenderThread() {
    Stop();
}

bool RenderThread::SamePiece(const uint8_t* a, const uint8_t* b) {
#if defined(__aarch64__)
    uint8x16_t diff = vdupq_n_u8(0);
    for (size_t i = 0; i < kPiece; i += 16) {
        diff = vorrq_u8(diff, veorq_u8(vld1q_u8(a + i), vld1q_u8(b + i)));
    }
    return vmaxvq_u8(diff) == 0;
#elif defined(__x86_64__) || defined(_M_X64)
    __m128i diff = _mm_setzero_si128();
    for (size_t i = 0; i < kPiece; i += 16) {
        diff = _mm_or_si128(
            diff,
            _mm_xor_si128(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i)),
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i))));
    }
    return _mm_movemask_epi8(_mm_cmpeq_epi8(diff, _mm_setzero_si128())) == 0xFFFF;
#else
    return std::memcmp(a, b, kPiece) == 0;
#endif
}

const std::vector<StateRange>& RenderThread::ActiveStateRanges() {
    static const std::vector<StateRange> kRanges = {
        {0, sizeof(GfxState)},
    };
    return kRanges;
}

void RenderThread::Start(BatchReplayFn replay_fn) {
    if (running_.exchange(true)) return;
    replay_fn_ = std::move(replay_fn);
    header_ring_.assign(kHeaderRingBytes, 0);
    ring_cursor_ = 0;
    pending_batch_.commands.reserve(kBatchCommands);
    worker_ = std::thread([this] { WorkerMain(); });
}

void RenderThread::Stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        queue_cv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
}

uint32_t RenderThread::PushResourceHeader(const ResourceHeader& hdr) {
    if (ring_cursor_ + sizeof(ResourceHeader) > header_ring_.size()) {
        ring_cursor_ = 0;
    }
    const uint32_t off = static_cast<uint32_t>(ring_cursor_);
    std::memcpy(header_ring_.data() + off, &hdr, sizeof(ResourceHeader));
    ring_cursor_ += sizeof(ResourceHeader);
    return off;
}

void RenderThread::EmitStateDiff(const GfxState& current) {
    const auto* cur_bytes = reinterpret_cast<const uint8_t*>(&current);
    auto* prev_bytes = reinterpret_cast<uint8_t*>(&last_sent_state_);

    for (const auto& r : ActiveStateRanges()) {
        for (size_t off = r.offset; off < r.offset + r.size; off += kPiece) {
            if (!SamePiece(cur_bytes + off, prev_bytes + off)) {
                const uint32_t delta_off =
                    static_cast<uint32_t>(pending_batch_.state_deltas.size());
                pending_batch_.state_deltas.insert(
                    pending_batch_.state_deltas.end(),
                    cur_bytes + off, cur_bytes + off + kPiece);
                std::memcpy(prev_bytes + off, cur_bytes + off, kPiece);

                RenderCmd patch{};
                patch.type = CmdType::StatePatch;
                patch.arg0 = static_cast<uint32_t>(off);
                patch.arg1 = static_cast<uint32_t>(kPiece);
                patch.arg2 = delta_off;
                EnqueueCommand(patch);
            }
        }
    }
}

void RenderThread::EnqueueCommand(const RenderCmd& cmd) {
    pending_batch_.commands.push_back(cmd);
    if (pending_batch_.commands.size() >= kBatchCommands ||
        cmd.type == CmdType::Present ||
        cmd.type == CmdType::FenceSync) {
        FlushBatch();
    }
}

void RenderThread::FlushBatch() {
    if (pending_batch_.commands.empty()) return;
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        queue_.push_back(std::move(pending_batch_));
    }
    pending_batch_ = CmdBatch{};
    pending_batch_.commands.reserve(kBatchCommands);
    queue_cv_.notify_one();
}

uint64_t RenderThread::SignalFence() {
    const uint64_t id = next_fence_++;
    RenderCmd cmd{};
    cmd.type = CmdType::FenceSync;
    cmd.fence_id = id;
    EnqueueCommand(cmd);
    return id;
}

void RenderThread::WaitForFence(uint64_t fence_id) {
    FlushBatch();
    std::unique_lock<std::mutex> lock(queue_mu_);
    fence_cv_.wait(lock, [&] {
        return completed_fence_ >= fence_id || !running_.load(std::memory_order_acquire);
    });
}

void RenderThread::WorkerMain() {
    log::Info("CT2RenderThread", "Dedicated Vulkan render thread started");
    while (true) {
        CmdBatch batch;
        {
            std::unique_lock<std::mutex> lock(queue_mu_);
            queue_cv_.wait(lock, [&] {
                return !queue_.empty() || !running_.load(std::memory_order_acquire);
            });
            if (queue_.empty() && !running_.load(std::memory_order_acquire)) break;
            batch = std::move(queue_.front());
            queue_.pop_front();
        }

        if (replay_fn_) {
            replay_fn_(batch, header_ring_.data());
        }

        uint64_t max_fence = 0;
        for (const auto& cmd : batch.commands) {
            if (cmd.type == CmdType::FenceSync && cmd.fence_id > max_fence) {
                max_fence = cmd.fence_id;
            }
        }
        if (max_fence > 0) {
            std::lock_guard<std::mutex> lock(queue_mu_);
            if (max_fence > completed_fence_) {
                completed_fence_ = max_fence;
                fence_cv_.notify_all();
            }
        }
    }
    log::Info("CT2RenderThread", "Dedicated Vulkan render thread stopped");
}

} // namespace ct2::gfx
