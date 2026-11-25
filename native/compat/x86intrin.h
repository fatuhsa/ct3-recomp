#pragma once
// Android on ARM64: XboxXBERecomp's runtime/cpu/dispatch.cpp includes <x86intrin.h> on non-Windows hosts;
// none of its x86 intrinsics are used on ARM64 (guest timers run on std::chrono).
