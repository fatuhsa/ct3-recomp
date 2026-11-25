#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>

namespace ct2::host {

// Unrooted hang watchdog:
// When foregrounded and no frame presents for ~10s, sends SIGUSR2 samples to
// registered guest/render threads to log host PC/LR as offsets in libmain.so
// (symbolizable via the unstripped copy) plus guest register snapshots.
class HangWatchdog {
public:
    static constexpr uint32_t kHangThresholdMs = 10000;
    static constexpr int kSampleCount = 6;

    struct TrackedThread {
        std::string name;
        pthread_t tid;
        uint32_t* guest_pc_ptr = nullptr;
    };

    HangWatchdog() = default;
    ~HangWatchdog();

    void Start(const std::string& log_dir);
    void Stop();

    void SetForeground(bool fg);
    void NotifyPresent();
    void RegisterThread(const std::string& name, pthread_t tid, uint32_t* guest_pc_ptr = nullptr);
    void UnregisterThread(pthread_t tid);

private:
    void WatchLoop();
    void CaptureHangDump();

    std::atomic<bool> running_{false};
    std::atomic<bool> foreground_{true};
    std::atomic<uint64_t> last_present_ms_{0};
    std::atomic<bool> dumped_for_current_hang_{false};

    std::mutex threads_mu_;
    std::vector<TrackedThread> threads_;
    std::string log_dir_;
    std::thread worker_;
};

} // namespace ct2::host
