# Crazy Taxi 3: High Roller — Android ARM64 + Vulkan Static Recompilation

[![Android ARM64 Vulkan CI](https://github.com/fatuhsa/ct3-recomp/actions/workflows/android-ci.yml/badge.svg)](https://github.com/fatuhsa/ct3-recomp/actions/workflows/android-ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

Native Android (`arm64-v8a`) + Vulkan static recompilation port of **Crazy Taxi 3: High Roller (USA)** (Original Xbox, TitleID `0x53450004`, XDK `1.0.4531`).

This port lifts the Original Xbox `default.xbe` into 128 native C++ translation units using [`XboxXBERecomp`](https://github.com/TGP482/XboxXBERecomp) and runs them natively on Android ARM64 with a mobile-optimized Vulkan 1.1–1.3 renderer and ARM64 NEON render thread adapted from [**RanggaCasper**](https://github.com/RanggaCasper)'s Android static recompilation ports.

> **Legal Notice:** This repository contains **no copyrighted game assets, ROMs, ISOs, or Xbox SDK binaries**. You must provide your own legally dumped USA Original Xbox copy of *Crazy Taxi 3: High Roller*. *Crazy Taxi 3: High Roller* is © SEGA / Hitmaker.

---

## Features

- **Native ARM64 Static Recompilation (128 Partitions):** Full x86-to-C++ lifting of `default.xbe` (`XDK 1.0.4531`) compiled at `-O2` with 16 KB ELF LOAD alignment for Android 15+.
- **Full 3D Mobile Vulkan Renderer (Vulkan 1.1 / 1.2 / 1.3):**
  - Handles Crazy Taxi 3's pre-baked NV2A indexed pushbuffer draws (`NV097_ARRAY_ELEMENT16` `0x1800` & `NV097_ARRAY_ELEMENT32` `0x1808`), `D3DDevice_SetVertexShaderInput`, `D3DDevice_SetModelView`, `D3DDevice_SetVertexBlendModelView`, and `D3DDevice_GetProjectionViewportMatrix`.
  - **Hardware 2/3/4-Bone Vertex Skinning (`D3DRS_VERTEXBLEND`):** Full GPU vertex blending via `b0` (`WORLD0..WORLD3`) for character joints and vehicle chassis deformation.
  - **Mobile GPU Quirk Handling (Mali / Adreno / PowerVR):**
    - ARM Mali VUID-06924 HLSL opaque-parameter specialization (`vk_shader.cpp`).
    - Core Vulkan 1.2+ timeline semaphores with automatic `VkFence` fallback on Vulkan 1.1.
    - Non-BC GPU texture VRAM cap (`1024` textures & `384 MB` LRU eviction for CPU-decoded DXT→BGRA).
    - Adreno (`0x5143`) cached `VkRenderPass` + async `VkCommandPool` reset.
    - Cross-frame `VkDescriptorSet` cache, zero-malloc per-frame static VB upload dedup, `MemSig` sampled surface hashing, atomic SPIR-V disk cache, and background `preload.bin` / `VkPipelineCache` warming.
- **ARM64 NEON Decoupled Render Thread:** Dedicated Vulkan render thread with 64-byte NEON state diffing (`vld1q_u8`/`veorq_u8`/`vorrq_u8`/`vmaxvq_u8`) and 1 MB header ring.
- **Native Android Audio & Controls:**
  - 48 kHz low-latency stereo **AAudio** output for Xbox DirectSound.
  - Classic Xbox **ABXY diamond** touch overlay (`Y`, `X`, `B`, `A`, `LT`, `RT`, `D-Pad`, `START`, `BACK`) + full Bluetooth/USB hardware gamepad support.
- **In-Game Settings Overlay (`SET`) & Live Performance HUD (`HUD`):**
  - Live internal resolution scaling (`50%`–`200%`), Vulkan API cap (`Auto 1.3` / `1.2` / `1.1`), Render Path (`Auto` / `Dynamic` / `Cached RP`), VSync, FPS Cap, Shadows, Fog, Bloom, Screen Filters (`Sharpen`, `Vivid`, `Scanlines`, `CRT`, `HDR`), `FXAA`, Shader Preload, Touch Opacity/Visibility, and real-time CPU/GPU/draw telemetry.
- **Direct XISO Streaming:** Streams all 401 Xbox disc files directly from the `.iso` via `/sdcard/crazy taxi` or Android Storage Access Framework (SAF) without manual extraction.

---

## Verified Game Image

| Field | Value |
|---|---|
| **Title** | Crazy Taxi 3: High Roller (USA) |
| **Title ID** | `0x53450004` |
| **Format** | Original Xbox XISO (`MICROSOFT*XBOX*MEDIA`, trimmed or Redump full DVD) |
| **XDK Version** | `1.0.4531` (`XAPILIB`, `D3D8`, `XGRAPHC`, `DSOUND`, `XBOXKRNL`, `LIBCMT`) |
| **`default.xbe` Size** | `2,179,072` bytes |
| **`default.xbe` SHA-1** | `10096ce907b92a702b52e4ccb3cf15ac167ccb6f` |

---

## Installation & Usage (Android)

1. Install the built `arm64-v8a` APK on an Android 10+ (`minSdk 29`) device with Vulkan 1.1+.
2. Place your legally dumped USA `.iso` in either:
   - `/sdcard/crazy taxi/Crazy Taxi 3 - High Roller (USA).iso`, **or**
   - Pick the `.iso` directly via the built-in **SAF file picker** on first launch.
3. **Default Xbox Controls:**
   - **RT:** Gas / Accelerate
   - **LT:** Brake
   - **B:** Drive Gear (`D`)
   - **A:** Reverse Gear (`R`) / Menu Confirm
   - **Y:** Crazy Hop
   - **D-Pad / Left Stick:** Steer
   - **SET (top-right):** Open Settings Overlay (Resolution, Vulkan version, VSync, graphics filters, touch opacity)
   - **HUD (top-right):** Toggle live FPS / GPU / profiler overlay

---

## Building from Source / GitHub Actions CI

### Automated CI Build (`.github/workflows/android-ci.yml`)
1. Extract and encrypt your `default.xbe` locally (never commit raw `.iso` or `.xbe` files):
   ```bash
   ./tools/extract_and_encrypt_xbe.sh "/path/to/Crazy Taxi 3 - High Roller (USA).iso" "<strong-passphrase>"
   ```
2. Add `<strong-passphrase>` as the GitHub Actions repository secret `XBE_KEY`.
3. Run the **Android ARM64 Vulkan CI** workflow. It will:
   - Fetch the portable `sdk/` runtime and build `xbe_recomp` (`Clang` + `Zydis` + `toml++`).
   - Lift `default.xbe` using [`config/game.toml`](config/game.toml) and [`config/xdk/4531/layout.toml`](config/xdk/4531/layout.toml) into 128 C++ partitions.
   - Build the single `arm64-v8a` APK (`Gradle 9.9.2`, `AGP 9.5.2`, `NDK r27b`, `glslang 16.0.0`, native `-O2`).
   - Verify 16 KB ELF LOAD alignment (`readelf -lW libmain.so`) for Android 15+ compatibility.

---

## Project Structure

```text
├── .github/workflows/android-ci.yml   # End-to-end lift + Android ARM64 APK CI
├── CMakeLists.txt                     # Native CMake build (128 recomp partitions + Vulkan host)
├── config/
│   ├── game.toml                      # Crazy Taxi 3 XBE symbol map & 128-partition config
│   └── xdk/4531/layout.toml           # XDK 1.0.4531 D3D8/CDevice struct layout
├── android/                           # Android app (MainActivity, Settings Overlay, SAF, CrashLogs)
├── native/
│   ├── ct3_gpu_backend.cpp            # Mobile Vulkan 1.1–1.3 backend + 4-bone vertex skinning
│   ├── ct3_app_host.cpp               # Android host layer (AAudio, input, settings, pacing, kernel hooks)
│   ├── vk_shader.cpp                  # glslang HLSL->SPIR-V compiler + Mali VUID-06924 Specialize()
│   ├── android_jni.cpp                # JNI bridge & live HUD telemetry
│   └── compat/                        # Android NDK & minimal host shims
├── src/
│   ├── d3d/gpu_render_thread.cpp      # Decoupled render thread + ARM64 NEON 64-byte state diffing
│   ├── host/                          # XISO parser, RomVfs, HangWatchdog, SaveGuard
│   └── kernel/                        # Xbox memory/kernel helpers
└── tools/extract_and_encrypt_xbe.sh   # Local helper to extract/verify/encrypt default.xbe
```

---

## Credits & References

This project would not exist without the pioneering static recompilation and mobile Vulkan work of the open-source community. See [`CREDITS.md`](CREDITS.md) for full details and third-party notices.

- **[RanggaCasper](https://github.com/RanggaCasper)** — Mobile Android ARM64 + Vulkan static recompilation architecture, portable Vulkan backend (`native/vulkan.cpp`), ARM64 NEON render thread (`src/d3d/render_thread.cpp`), Mali/Adreno/PowerVR driver workarounds, shader preloading, and performance optimization patterns derived from his Android ports:
  - [`MostWanted-Android`](https://github.com/RanggaCasper/MostWanted-Android) (GPL-3.0)
  - [`Black-Android`](https://github.com/RanggaCasper/Black-Android)
  - [`ShaolinMonks-Android`](https://github.com/RanggaCasper/ShaolinMonks-Android)
  - [`DW5-Android`](https://github.com/RanggaCasper/DW5-Android)
  - [`DEFJAM-XBERecomp-Android`](https://github.com/RanggaCasper/DEFJAM-XBERecomp-Android)
- **[TGP482](https://github.com/TGP482)** — [`XboxXBERecomp`](https://github.com/TGP482/XboxXBERecomp) (MIT License), the Original Xbox x86-to-C++ static recompiler (`xbe_recomp`) and Xbox runtime SDK (`sdk/`).
- **Khronos Group** (`glslang`), **Zyantific** (`Zydis`), **Mark Gillard** (`toml++`), and the **xemu / NV2A** documentation community for Original Xbox GPU register references.

## License

This repository is licensed under the **GNU General Public License v3.0** — see [`LICENSE`](LICENSE) for details.
