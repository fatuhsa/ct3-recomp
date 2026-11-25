#pragma once

#include "host/rom_vfs.h"
#include "host/xbe_loader.h"

#include <cstdint>

namespace ct2::xbox {

// Resolves and binds all 121 xboxkrnl.exe ordinals imported by Crazy Taxi 3: High Roller (USA)
// (TitleID 0x53450004, XDK 4531, KernelThunk VA 0x0012A560).
class XboxKernel {
public:
    bool Initialize(host::RomVfs* rom, const host::XbeImageInfo& xbe);
    void Shutdown();

    // Returns the official xboxkrnl.exe symbol name for an ordinal (or "Ordinal_N").
    static const char* OrdinalName(uint32_t ordinal);

    // Dispatches a kernel thunk call by ordinal (used by recompiled guest code).
    uint32_t DispatchThunk(uint32_t ordinal, uint32_t* guest_stack_args);

    size_t BoundThunkCount() const { return bound_thunks_; }

private:
    host::RomVfs* rom_ = nullptr;
    size_t bound_thunks_ = 0;
};

} // namespace ct2::xbox
