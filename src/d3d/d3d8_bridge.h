#pragma once

#include "gfx/render_thread.h"

#include <cstdint>

namespace ct2::d3d {

// Bridge between recompiled Xbox D3D8 (XDK 4531, section VA 0x000F3800) calls
// and the decoupled NEON-diffed RenderThread -> Mobile Vulkan renderer.
class D3D8Bridge {
public:
    explicit D3D8Bridge(gfx::RenderThread* rt);

    void SetRenderTarget(uint32_t color_va, uint32_t depth_va);
    void SetTexture(uint32_t stage, uint32_t tex_va, uint16_t w, uint16_t h, uint16_t fmt,
                    uint32_t byte_size);
    void SetVertexShader(uint32_t shader_handle);
    void SetPixelShader(uint32_t shader_handle);
    void SetRenderState(uint32_t blend, uint32_t depth_stencil, uint32_t raster);
    void SetTransform(const float mvp16[16]);

    void Clear(uint32_t argb_color);
    void DrawIndexedVertices(uint32_t topology, uint32_t vertex_count, uint32_t vertex_stride,
                             uint32_t vb_va, uint32_t vb_bytes);
    void Swap();
    void BlockUntilIdle();

    const gfx::GfxState& State() const { return state_; }

private:
    gfx::RenderThread* rt_ = nullptr;
    gfx::GfxState state_{};
    uint32_t active_tex_ring_offset_ = 0;
};

} // namespace ct2::d3d
