# Credits & Third-Party Notices

This project builds directly upon open-source static recompilation tools, mobile Vulkan renderer engineering, and Original Xbox hardware documentation from the following authors and projects.

---

## Primary Upstream Projects & References

### 1. RanggaCasper — Mobile Android ARM64 + Vulkan Static Recompilation Ports
- **Author:** [RanggaCasper](https://github.com/RanggaCasper)
- **Primary Reference Repository:** [`RanggaCasper/MostWanted-Android`](https://github.com/RanggaCasper/MostWanted-Android) (**GNU General Public License v3.0**)
- **Additional Reference Ports:**
  - [`RanggaCasper/Black-Android`](https://github.com/RanggaCasper/Black-Android)
  - [`RanggaCasper/ShaolinMonks-Android`](https://github.com/RanggaCasper/ShaolinMonks-Android)
  - [`RanggaCasper/DW5-Android`](https://github.com/RanggaCasper/DW5-Android)
  - [`RanggaCasper/DEFJAM-XBERecomp-Android`](https://github.com/RanggaCasper/DEFJAM-XBERecomp-Android)
- **Contributions Used / Adapted Here:**
  - Portable mobile Vulkan 1.1–1.3 renderer (`native/vulkan.cpp` → `native/ct3_gpu_backend.cpp`), including cross-frame `VkDescriptorSet` caching, Adreno cached `VkRenderPass` + async `VkCommandPool` reset, Mali Vulkan 1.2+ core timeline semaphores with `VkFence` fallback, non-BC 384 MB / 1024-texture VRAM LRU eviction, `MemSig` sampled surface hashing, and `preload.bin` / `VkPipelineCache` background warming.
  - Decoupled render thread with 64-byte ARM64 NEON state diffing and 1 MB header ring (`src/d3d/render_thread.cpp` → `src/d3d/gpu_render_thread.cpp`).
  - HLSL opaque-parameter specialization pattern (`Specialize()`) for ARM Mali SPIR-V VUID-06924 compatibility (`native/vk_shader.cpp`).
  - Portable Xbox runtime SDK integration, NDK compat shims (`native/compat/`), Android crash forensics (`CrashLogs.kt`), hang watchdog, save guard, dynamic resolution anti-oscillation logic, and Android 15+ 16 KB ELF alignment setup.

### 2. TGP482 — `XboxXBERecomp`
- **Author:** [TGP482](https://github.com/TGP482)
- **Repository:** [`TGP482/XboxXBERecomp`](https://github.com/TGP482/XboxXBERecomp) (**MIT License**)
- **Contributions Used:**
  - `xbe_recomp` x86-to-C++ static recompiler (`tools/recomp/`).
  - Portable Original Xbox kernel, XAPI, D3D8, XGraphics, DirectSound, and NV2A shader/vertex runtime (`sdk/runtime/`).

---

## Third-Party Libraries

| Library | Version | License | Purpose |
|---|---|---|---|
| [Khronos `glslang`](https://github.com/KhronosGroup/glslang) | `16.0.0` | BSD-3-Clause / Apache-2.0 | Runtime HLSL-to-SPIR-V shader compilation |
| [Zyantific `Zydis`](https://github.com/zyantific/zydis) | `v4.1.1` | MIT | x86 instruction decoder used by `xbe_recomp` |
| [Mark Gillard `toml++`](https://github.com/marzer/tomlplusplus) | `v3.4.0` | MIT | TOML parser for `config/game.toml` & XDK layouts |
| [AndroidX (`core-ktx`, `activity-ktx`)](https://developer.android.com/jetpack/androidx) | `1.13.1` / `1.9.3` | Apache-2.0 | Android Activity / SAF / BroadcastReceiver helpers |
| [Vulkan Headers & Android NDK / AAudio](https://developer.android.com/ndk) | NDK r27b | Apache-2.0 | Native ARM64 Vulkan & low-latency audio APIs |

---

## Hardware & Documentation References

- **[xemu](https://github.com/xemu-project/xemu)** & **envytools** — Original Xbox NV2A pushbuffer methods (`NV097_ARRAY_ELEMENT16` `0x1800`, `NV097_ARRAY_ELEMENT32` `0x1808`, `NV097_SET_SKIN_MODE` `0x0328`) and XDK D3D8 state reference.
- **XboxDevWiki** — XBE header layout, XISO (`MICROSOFT*XBOX*MEDIA`) filesystem specification, and `xboxkrnl.exe` ordinal tables.

---

## Trademark & Copyright Notice

*Crazy Taxi 3: High Roller* is a registered trademark and copyright of **SEGA Corporation / Hitmaker**. *Xbox* is a registered trademark of **Microsoft Corporation**. This project is not affiliated with, endorsed by, or sponsored by SEGA or Microsoft.
