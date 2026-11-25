#pragma once

// Minimal NDK/Linux compatibility header force-included into host sources only
// (never into generated recompiled guest partitions).

#include <cstdint>
#include <cstddef>

#if defined(__ANDROID__)
#include <sys/syscall.h>
#include <unistd.h>

// Polyfill memfd_create on Android NDK API < 30 (e.g. API 29)
#if defined(__ANDROID_API__) && (__ANDROID_API__ < 30)
#if defined(__NR_memfd_create)
static inline int ct2_memfd_create(const char* name, unsigned int flags) {
    return static_cast<int>(syscall(__NR_memfd_create, name, flags));
}
#ifndef memfd_create
#define memfd_create(name, flags) ct2_memfd_create((name), (flags))
#endif
#endif
#endif
#endif // __ANDROID__

// Lightweight x86intrin/SIMD portability guards for non-x86 hosts (ARM64)
#if defined(__aarch64__)
#include <arm_neon.h>
#define CT2_ARCH_ARM64 1
#elif defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#define CT2_ARCH_X86_64 1
#endif

// Explicit memory barriers for ARM64 weak memory ordering visibility
#if defined(__aarch64__)
#define CT2_DMB_ISH() __asm__ volatile("dmb ish" ::: "memory")
#define CT2_DMB_ISHST() __asm__ volatile("dmb ishst" ::: "memory")
#else
#define CT2_DMB_ISH() __sync_synchronize()
#define CT2_DMB_ISHST() __sync_synchronize()
#endif
