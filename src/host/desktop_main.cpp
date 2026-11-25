#include "d3d/d3d8_bridge.h"
#include "game/game_loop.h"
#include "gfx/render_thread.h"
#include "host/log.h"
#include "host/memory.h"
#include "host/rom_vfs.h"
#include "host/xbe_loader.h"
#include "kernel/xbox_kernel.h"
#include "native/stats.h"
#include "native/vulkan.h"
#include "recomp/x86_context.h"

#include <string>

int main(int argc, char** argv) {
    ct2::kernel::InitMemory(/*expand_ram_128mb=*/true);

    ct2::host::RomVfs rom;
    const std::string arg_path = (argc > 1) ? std::string(argv[1]) : std::string{};
    ct2::host::XbeImageInfo xbe{};
    ct2::xbox::XboxKernel kernel;

    if (rom.AutoDiscoverFromSdcard(arg_path) && rom.LoadDefaultXbe(xbe)) {
        kernel.Initialize(&rom, xbe);
    } else {
        ct2::log::Warn("CT3Main", "Running scaffold self-check without mounted Crazy Taxi 3 ISO");
    }

    ct2::diag::InitTelemetry(/*app_files_dir=*/".", /*rom_dir=*/rom.Info().root_dir);
    ct2::vk::VulkanRenderer renderer;
    ct2::vk::RendererConfig cfg{};
    cfg.cache_dir = "shadercache";
    renderer.Initialize(cfg);

    ct2::gfx::RenderThread rt;
    rt.Start([&](const ct2::gfx::CmdBatch& batch, const uint8_t* ring) {
        renderer.ReplayBatch(batch, ring);
    });
    ct2::d3d::D3D8Bridge d3d(&rt);

    ct2::game::GameLoop loop;
    ct2::recomp::X86Context ctx{};
    if (xbe.entry_point) ctx.eip = xbe.entry_point;

    for (uint64_t frame = 0; frame < 3; ++frame) {
        ct2::recomp::StepGuestFrame(ctx, frame);
        if (!loop.TickFrameSkip()) {
            d3d.Clear(0xFF102030u);
            d3d.Swap();
        }
    }

    d3d.BlockUntilIdle();
    rt.Stop();
    renderer.Shutdown();
    kernel.Shutdown();
    ct2::kernel::ShutdownMemory();
    return 0;
}
