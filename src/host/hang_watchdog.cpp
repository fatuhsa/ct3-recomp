#include "host/hang_watchdog.h"

#include "host/log.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <ucontext.h>

namespace ct2::host {

namespace {

struct SampleSlot {
    std::atomic<bool> ready{false};
    uintptr_t pc = 0;
    uintptr_t lr = 0;
    uintptr_t fp = 0;
};

SampleSlot g_sample;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void SigUsr2Handler(int, siginfo_t*, void* uctx_raw) {
    auto* uc = static_cast<ucontext_t*>(uctx_raw);
    uintptr_t pc = 0;
    uintptr_t lr = 0;
    uintptr_t fp = 0;
#if defined(__aarch64__) && defined(__linux__)
    pc = static_cast<uintptr_t>(uc->uc_mcontext.pc);
    lr = static_cast<uintptr_t>(uc->uc_mcontext.regs[30]);
    fp = static_cast<uintptr_t>(uc->uc_mcontext.regs[29]);
#elif defined(__x86_64__) && defined(__linux__)
    pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
    fp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RBP]);
#else
    (void)uc;
#endif
    g_sample.pc = pc;
    g_sample.lr = lr;
    g_sample.fp = fp;
    g_sample.ready.store(true, std::memory_order_release);
}

std::string FormatLibOffset(uintptr_t addr) {
    Dl_info info{};
    char buf[192];
    if (addr && dladdr(reinterpret_cast<const void*>(addr), &info) && info.dli_fname) {
        const uintptr_t rel = addr - reinterpret_cast<uintptr_t>(info.dli_fbase);
        const char* base = std::strrchr(info.dli_fname, '/');
        std::snprintf(buf, sizeof(buf), "%s+0x%zx",
                      base ? base + 1 : info.dli_fname, static_cast<size_t>(rel));
    } else {
        std::snprintf(buf, sizeof(buf), "0x%zx", static_cast<size_t>(addr));
    }
    return buf;
}

} // namespace

HangWatchdog::~HangWatchdog() {
    Stop();
}

void HangWatchdog::Start(const std::string& log_dir) {
    if (running_.exchange(true)) return;
    log_dir_ = log_dir;
    last_present_ms_.store(NowMs(), std::memory_order_release);
    dumped_for_current_hang_.store(false, std::memory_order_release);

    struct sigaction sa{};
    sa.sa_sigaction = SigUsr2Handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, nullptr);

    worker_ = std::thread([this] { WatchLoop(); });
}

void HangWatchdog::Stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
}

void HangWatchdog::SetForeground(bool fg) {
    foreground_.store(fg, std::memory_order_release);
    if (fg) {
        last_present_ms_.store(NowMs(), std::memory_order_release);
        dumped_for_current_hang_.store(false, std::memory_order_release);
    }
}

void HangWatchdog::NotifyPresent() {
    last_present_ms_.store(NowMs(), std::memory_order_release);
    dumped_for_current_hang_.store(false, std::memory_order_release);
}

void HangWatchdog::RegisterThread(const std::string& name, pthread_t tid,
                                  uint32_t* guest_pc_ptr) {
    std::lock_guard<std::mutex> lock(threads_mu_);
    threads_.push_back(TrackedThread{name, tid, guest_pc_ptr});
}

void HangWatchdog::UnregisterThread(pthread_t tid) {
    std::lock_guard<std::mutex> lock(threads_mu_);
    threads_.erase(std::remove_if(threads_.begin(), threads_.end(),
                                  [tid](const TrackedThread& t) {
                                      return pthread_equal(t.tid, tid) != 0;
                                  }),
                   threads_.end());
}

void HangWatchdog::WatchLoop() {
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!foreground_.load(std::memory_order_acquire)) continue;

        const uint64_t elapsed = NowMs() - last_present_ms_.load(std::memory_order_acquire);
        if (elapsed >= kHangThresholdMs && !dumped_for_current_hang_.exchange(true)) {
            CaptureHangDump();
        }
    }
}

void HangWatchdog::CaptureHangDump() {
    std::vector<TrackedThread> snapshot;
    {
        std::lock_guard<std::mutex> lock(threads_mu_);
        snapshot = threads_;
    }

    std::error_code ec;
    if (!log_dir_.empty()) std::filesystem::create_directories(log_dir_, ec);
    const std::string dump_path =
        log_dir_.empty() ? "hang_dump.txt" : (log_dir_ + "/hang_dump.txt");

    std::ofstream out(dump_path, std::ios::app);
    out << "=== CT2 Hang Watchdog Triggered (>=" << kHangThresholdMs << "ms without present) ===\n";
    log::Error("CT2Hang", "No frame presented for >= %u ms; sampling %zu threads",
               kHangThresholdMs, snapshot.size());

    for (const auto& t : snapshot) {
        const uint32_t gpc = t.guest_pc_ptr ? *t.guest_pc_ptr : 0u;
        out << "Thread '" << t.name << "' guest_pc=0x" << std::hex << gpc << std::dec << "\n";
        for (int s = 0; s < kSampleCount; ++s) {
            g_sample.ready.store(false, std::memory_order_release);
            if (pthread_kill(t.tid, SIGUSR2) != 0) break;

            for (int wait = 0; wait < 20 && !g_sample.ready.load(std::memory_order_acquire); ++wait) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!g_sample.ready.load(std::memory_order_acquire)) continue;

            const std::string pc_str = FormatLibOffset(g_sample.pc);
            const std::string lr_str = FormatLibOffset(g_sample.lr);
            out << "  sample[" << s << "] pc=" << pc_str << " lr=" << lr_str << "\n";
            log::Error("CT2Hang", "  [%s #%d] pc=%s lr=%s",
                       t.name.c_str(), s, pc_str.c_str(), lr_str.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }
    out << "\n";
}

} // namespace ct2::host
