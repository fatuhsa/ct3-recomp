#include "d3d/d3d8_bridge.h"

#include <cstring>

namespace ct2::d3d {

D3D8Bridge::D3D8Bridge(gfx::RenderThread* rt) : rt_(rt) {}

void D3D8Bridge::SetRenderTarget(uint32_t color_va, uint32_t depth_va) {
    state_.render_target_va = color_va;
    state_.depth_target_va = depth_va;
}

void D3D8Bridge::SetTexture(uint32_t stage, uint32_t tex_va, uint16_t w, uint16_t h,
                            uint16_t fmt, uint32_t byte_size) {
    if (stage >= state_.tex_va.size()) return;
    state_.tex_va[stage] = tex_va;
    if (rt_ && tex_va) {
        gfx::ResourceHeader hdr{};
        hdr.guest_va = tex_va;
        hdr.byte_size = byte_size;
        hdr.width = w;
        hdr.height = h;
        hdr.format = fmt;
        active_tex_ring_offset_ = rt_->PushResourceHeader(hdr);
    }
}

void D3D8Bridge::SetVertexShader(uint32_t shader_handle) {
    state_.shader_key = (state_.shader_key & 0xFFFF0000u) | (shader_handle & 0xFFFFu);
}

void D3D8Bridge::SetPixelShader(uint32_t shader_handle) {
    state_.shader_key = (state_.shader_key & 0x0000FFFFu) | ((shader_handle & 0xFFFFu) << 16);
}

void D3D8Bridge::SetRenderState(uint32_t blend, uint32_t depth_stencil, uint32_t raster) {
    state_.blend_state = blend;
    state_.depth_stencil_state = depth_stencil;
    state_.raster_state = raster;
}

void D3D8Bridge::SetTransform(const float mvp16[16]) {
    if (mvp16) std::memcpy(state_.mvp.data(), mvp16, sizeof(state_.mvp));
}

void D3D8Bridge::Clear(uint32_t argb_color) {
    if (!rt_) return;
    rt_->EmitStateDiff(state_);
    gfx::RenderCmd cmd{};
    cmd.type = gfx::CmdType::Clear;
    cmd.arg0 = argb_color;
    rt_->EnqueueCommand(cmd);
}

void D3D8Bridge::DrawIndexedVertices(uint32_t topology, uint32_t vertex_count,
                                     uint32_t vertex_stride, uint32_t vb_va, uint32_t vb_bytes) {
    if (!rt_) return;
    state_.topology = topology;
    state_.vertex_stride = vertex_stride;
    rt_->EmitStateDiff(state_);

    gfx::RenderCmd cmd{};
    cmd.type = gfx::CmdType::DrawIndexed;
    cmd.arg0 = vertex_count;
    cmd.arg1 = vb_va;
    cmd.arg2 = vb_bytes;
    cmd.header_ring_offset = active_tex_ring_offset_;
    rt_->EnqueueCommand(cmd);
}

void D3D8Bridge::Swap() {
    if (!rt_) return;
    rt_->EmitStateDiff(state_);
    gfx::RenderCmd cmd{};
    cmd.type = gfx::CmdType::Present;
    rt_->EnqueueCommand(cmd);
}

void D3D8Bridge::BlockUntilIdle() {
    if (!rt_) return;
    const uint64_t fence = rt_->SignalFence();
    rt_->WaitForFence(fence);
}

} // namespace ct2::d3d
