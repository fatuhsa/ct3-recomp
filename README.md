# Crazy Taxi 3: High Roller (USA) — Original Xbox XBE → Android ARM64 + Vulkan

Native Android (`arm64-v8a`) + Vulkan static recompilation project for **Crazy Taxi 3: High Roller (USA)** (Original Xbox, TitleID `0x53450004`, XDK `1.0.4531`), implementing mobile GPU and ARM64 host patterns from `mobile-recomp-vulkan-opt` and integrating `TGP482/XboxXBERecomp`.

## Supported Game Image (`Crazy Taxi 3 - High Roller (USA).iso`)

- **Format:** Original Xbox XISO (`MICROSOFT*XBOX*MEDIA`, trimmed or Redump full DVD)
- **Executable:** `default.xbe` (`2,179,072` bytes, `SHA-1: 10096ce907b92a702b52e4ccb3cf15ac167ccb6f`)
- **XBE Metadata:** `TitleID = 0x53450004`, `Base = 0x00010000`, `Entry = 0x000E0C34`, `KernelThunk = 0x0012A560` (121 `xboxkrnl.exe` ordinals), XDK `1.0.4531` (`XAPILIB`, `D3D8`, `XGRAPHC`, `DSOUND`, `XBOXKRNL`, `LIBCMT`)

## ROM Location on Android (`/sdcard/crazy taxi`)

At launch, the APK streams all 401 Xbox disc files (`default.xbe`, `Media/ADX/adx0.afs`, `Media/bank*.art`, `Media/Collision/*`, etc.) directly from the XISO image without manual extraction:

1. `/sdcard/crazy taxi/Crazy Taxi 3 - High Roller (USA).iso` (and case variants under `/sdcard/Crazy Taxi`, `/sdcard/ct3`, or app `files/rom/`).
2. **Android Storage Access Framework (SAF)** file picker fallback — persists URI permission and passes a detached file descriptor (`fd`) to native `RomVfs` (`pread()` + mutex).

## Architecture & Mobile Optimizations

| Layer | Files | Key Patterns Implemented |
|---|---|---|
| **XISO / XBE / Kernel** | `src/host/xiso.*`, `src/host/xbe_loader.*`, `src/host/rom_vfs.*`, `src/host/memory.*`, `src/kernel/xbox_kernel.*` | Streaming `MICROSOFT*XBOX*MEDIA` parser (trimmed & Redump offset), XBE section/thunk loader, 64/128 MB Xbox RAM + dirty-page epochs, all 121 Crazy Taxi 3 `xboxkrnl.exe` ordinals |
| **Static Recompiler Config** | `config/game.toml`, `config/xdk/4531/layout.toml`, `tools/extract_and_encrypt_xbe.sh` | `TGP482/XboxXBERecomp` (`xbe_recomp`) 128-partition config for XDK 4531 + helper to extract/encrypt `ci/default.xbe.enc` for CI |
| **D3D8 & NEON Render Thread** | `src/d3d/d3d8_bridge.*`, `src/gfx/render_thread.*`, `src/recomp/x86_context.h` | Decoupled title/GPU threads (`kBatchCommands = 64`), 1 MB header ring, 64-byte **ARM64 NEON** (`vld1q_u8`/`veorq_u8`/`vorrq_u8`/`vmaxvq_u8`) state diffing |
| **Mobile Vulkan** | `native/vulkan.*`, `native/vk_shader.*` | Adreno (`0x5143`) cached `VkRenderPass` + async `VkCommandPool` reset; Mali VK 1.2-only timeline semaphores + fence fallback + HLSL opaque-param `Specialize()` (VUID-06924); non-BC VRAM count+byte cap (`1024` / `384 MB`); cross-frame `VkDescriptorSet` cache + `view_gen_`; triple-buffered `32 MB` upload ring; `MemSig` sampled hashing; atomic SPIR-V cache + periodic `VkPipelineCache` save (every 64 PSOs) + `preload.bin` background warming; `24 MB/frame` texture streaming budget |
| **Adaptive Perf & Telemetry** | `native/stats.*`, `native/dynamic_resolution.*`, `src/game/game_loop.*` | Toggleable `perf` marker file + `PerfScope` + `VkQueryPool` GPU timestamps + 600-frame pass table; `/proc/self/stat` CPU & `BatteryManager` temp fallbacks; anti-oscillation dynamic resolution (`sqrt(fps/target)`, 30s ceiling); fixed-timestep 60 Hz frame skip (`<= 3` skips); engine-level graphics switches |
| **Build, CI & Android App** | `CMakeLists.txt`, `android/*`, `.github/workflows/android-ci.yml` | `-O2 -w -fno-strict-aliasing -fsigned-char` on recomp partitions even in debug APKs; `-Wl,-z,max-page-size=16384` verified via `readelf -lW`; `-Pdebuggable` for unrooted `simpleperf`; Android 11+ `ApplicationExitInfo` crash dumper; zero-deprecated GitHub Actions (`checkout@v7`, `setup-java@v6`, `setup-gradle@v6`, `upload-artifact@v7`) |

## Lifting `default.xbe` in GitHub Actions CI

No copyrighted game files are stored in the repository. To enable automated `XboxXBERecomp` lifting in CI:

1. Run `./tools/extract_and_encrypt_xbe.sh "/root/Crazy Taxi 3 - High Roller (USA).iso" "<strong-passphrase>"` to generate `ci/default.xbe.enc`.
2. Add `<strong-passphrase>` as the GitHub repository secret `XBE_KEY`.
3. Trigger `Android ARM64 Vulkan CI` (with `run_xbe_recomp = true` or push `ci/default.xbe.enc`). CI decrypts & SHA-1-verifies `default.xbe`, builds `xbe_recomp` from `TGP482/XboxXBERecomp`, lifts 128 C++ partitions into `generated/game/`, builds the `arm64-v8a` APK, and checks 16 KB ELF alignment.
