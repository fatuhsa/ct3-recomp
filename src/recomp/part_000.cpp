#include "recomp/x86_context.h"

namespace ct2::recomp {

// Placeholder partition 000 used when building the runtime before lifting default.xbe.
// When CI or local build runs XboxXBERecomp against Crazy Taxi 3 (USA) default.xbe
// (SHA-1: 10096ce907b92a702b52e4ccb3cf15ac167ccb6f), 128 generated partitions replace this stub.
void StepGuestFrame(X86Context& ctx, uint64_t frame_index) {
    ctx.eax = static_cast<uint32_t>(frame_index);
    ctx.eip = 0x000E0C34u + static_cast<uint32_t>((frame_index & 0xFFu) * 4u);
}

} // namespace ct2::recomp
