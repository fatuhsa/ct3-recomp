#include "host/memory.h"

#include "host/log.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace ct2::kernel {

namespace {

std::vector<uint8_t> g_ram;
std::vector<uint64_t> g_epochs;
std::atomic<uint64_t> g_global_epoch{1};

bool ResolveOffset(uint32_t va, uint32_t* out_off) {
    if (g_ram.empty()) return false;
    // Strip Xbox kernel/contiguous uncached alias bits (0x80000000 / 0xF0000000 -> physical)
    uint32_t phys = va;
    if (phys >= 0x80000000u && phys < 0x80000000u + g_ram.size()) {
        phys -= 0x80000000u;
    }
    if (phys >= g_ram.size()) return false;
    *out_off = phys;
    return true;
}

} // namespace

bool InitMemory(bool expand_ram_128mb) {
    const size_t ram_bytes = expand_ram_128mb ? kXboxDevkitRamSize : kXboxRetailRamSize;
    g_ram.assign(ram_bytes, 0);
    g_epochs.assign((ram_bytes >> kDirtyPageShift) + 1, 1);
    g_global_epoch.store(1, std::memory_order_release);

    log::Info("CT3Mem", "Initialized Original Xbox guest RAM=%zu MB (%s layout)",
              ram_bytes >> 20, expand_ram_128mb ? "128 MB devkit/mod" : "64 MB retail");
    return true;
}

void ShutdownMemory() {
    g_ram.clear();
    g_epochs.clear();
}

uint32_t RamSizeBytes() {
    return static_cast<uint32_t>(g_ram.size());
}

void* HostPtr(uint32_t guest_va) {
    uint32_t off = 0;
    if (!ResolveOffset(guest_va, &off)) return nullptr;
    return g_ram.data() + off;
}

const void* HostPtrConst(uint32_t guest_va) {
    return HostPtr(guest_va);
}

uint8_t R8(uint32_t va) {
    const auto* p = static_cast<const uint8_t*>(HostPtrConst(va));
    return p ? *p : 0;
}

uint16_t R16(uint32_t va) {
    const auto* p = static_cast<const uint8_t*>(HostPtrConst(va));
    if (!p) return 0;
    uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint32_t R32(uint32_t va) {
    const auto* p = static_cast<const uint8_t*>(HostPtrConst(va));
    if (!p) return 0;
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint64_t R64(uint32_t va) {
    const auto* p = static_cast<const uint8_t*>(HostPtrConst(va));
    if (!p) return 0;
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void W8(uint32_t va, uint8_t v) {
    auto* p = static_cast<uint8_t*>(HostPtr(va));
    if (!p) return;
    *p = v;
    MarkDirtyRange(va, 1);
}

void W16(uint32_t va, uint16_t v) {
    auto* p = static_cast<uint8_t*>(HostPtr(va));
    if (!p) return;
    std::memcpy(p, &v, sizeof(v));
    MarkDirtyRange(va, sizeof(v));
}

void W32(uint32_t va, uint32_t v) {
    auto* p = static_cast<uint8_t*>(HostPtr(va));
    if (!p) return;
    std::memcpy(p, &v, sizeof(v));
    MarkDirtyRange(va, sizeof(v));
}

void W64(uint32_t va, uint64_t v) {
    auto* p = static_cast<uint8_t*>(HostPtr(va));
    if (!p) return;
    std::memcpy(p, &v, sizeof(v));
    MarkDirtyRange(va, sizeof(v));
}

void MarkDirtyRange(uint32_t va, uint32_t bytes) {
    uint32_t off = 0;
    if (!ResolveOffset(va, &off) || bytes == 0) return;

    const uint64_t epoch = g_global_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    const size_t first_page = off >> kDirtyPageShift;
    const size_t last_byte = std::min(g_ram.size() - 1, static_cast<size_t>(off) + bytes - 1);
    const size_t last_page = last_byte >> kDirtyPageShift;
    for (size_t pg = first_page; pg <= last_page && pg < g_epochs.size(); ++pg) {
        g_epochs[pg] = epoch;
    }
}

uint64_t DirtyEpoch(uint32_t va, uint32_t bytes) {
    uint32_t off = 0;
    if (!ResolveOffset(va, &off) || bytes == 0) return 0;

    const size_t first_page = off >> kDirtyPageShift;
    const size_t last_byte = std::min(g_ram.size() - 1, static_cast<size_t>(off) + bytes - 1);
    const size_t last_page = last_byte >> kDirtyPageShift;
    uint64_t max_epoch = 0;
    for (size_t pg = first_page; pg <= last_page && pg < g_epochs.size(); ++pg) {
        max_epoch = std::max(max_epoch, g_epochs[pg]);
    }
    return max_epoch;
}

} // namespace ct2::kernel
