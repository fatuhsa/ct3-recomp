#pragma once

#include <cstdint>

namespace ct2::recomp {

// Original Xbox (Pentium III Coppermine x86 + x87 + SSE/MMX) guest CPU context
// aligned with XboxXBERecomp static recompilation conventions.
struct alignas(16) X86Context {
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    uint32_t esi = 0;
    uint32_t edi = 0;
    uint32_t ebp = 0;
    uint32_t esp = 0x03FF0000u; // Initial guest stack near top of 64 MB retail RAM
    uint32_t eip = 0x000E0C34u; // Crazy Taxi 3: High Roller (USA) retail entry point
    uint32_t eflags = 0x00000202u;

    // x87 FPU stack (tracked at runtime) + control word
    double st[8]{};
    uint32_t fpu_top = 0;
    uint16_t fpu_cw = 0x027Fu;
    uint16_t fpu_sw = 0;

    // SSE XMM0..XMM7 registers
    alignas(16) float xmm[8][4]{};
};

// Entry point implemented by generated partitions under src/recomp/part_*.cpp
// (or generated/game/ when lifted by TGP482/XboxXBERecomp).
void StepGuestFrame(X86Context& ctx, uint64_t frame_index);

} // namespace ct2::recomp
