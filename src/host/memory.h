#pragma once

#include <cstddef>
#include <cstdint>

namespace ct2::kernel {

// Original Xbox flat 32-bit physical/virtual memory layout:
//  - Retail Xbox: 64 MB unified RAM (0x00000000 .. 0x03FFFFFF)
//  - Devkit / Mod-expanded mode: 128 MB RAM (0x00000000 .. 0x07FFFFFF)
//  - Kernel contiguous/uncached aliases (0x80000000+) mask cleanly into physical RAM.
constexpr uint32_t kXboxRetailRamSize = 64u << 20;
constexpr uint32_t kXboxDevkitRamSize = 128u << 20;
constexpr uint32_t kXbeDefaultBase = 0x00010000u;
constexpr uint32_t kDirtyPageShift = 12; // 4 KB dirty-tracking pages

bool InitMemory(bool expand_ram_128mb = true);
void ShutdownMemory();

uint32_t RamSizeBytes();

// Translate Xbox guest virtual/physical address to host pointer (nullptr if out of bounds).
void* HostPtr(uint32_t guest_va);
const void* HostPtrConst(uint32_t guest_va);

uint8_t R8(uint32_t va);
uint16_t R16(uint32_t va);
uint32_t R32(uint32_t va);
uint64_t R64(uint32_t va);

void W8(uint32_t va, uint8_t v);
void W16(uint32_t va, uint16_t v);
void W32(uint32_t va, uint32_t v);
void W64(uint32_t va, uint64_t v);

// Dirty-surface epoch tracking used by Vulkan MemSig fast-path.
void MarkDirtyRange(uint32_t va, uint32_t bytes);
uint64_t DirtyEpoch(uint32_t va, uint32_t bytes = 4096);

} // namespace ct2::kernel
