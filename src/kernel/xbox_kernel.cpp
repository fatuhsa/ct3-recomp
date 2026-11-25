#include "kernel/xbox_kernel.h"

#include "host/log.h"
#include "host/memory.h"

#include <chrono>
#include <cstdio>
#include <unordered_map>

namespace ct2::xbox {

namespace {

// Synthetic guest thunk stub address base patched into XBE KernelThunk table (0x0012A560).
constexpr uint32_t kThunkStubBase = 0xFE000000u;

const std::unordered_map<uint32_t, const char*>& KnownOrdinals() {
    // Exact 121 xboxkrnl.exe ordinals imported by Crazy Taxi 3: High Roller (USA) default.xbe
    static const std::unordered_map<uint32_t, const char*> kMap = {
        {1, "AvGetSavedDataAddress"},
        {2, "AvSendTVEncoderOption"},
        {3, "AvSetDisplayMode"},
        {4, "AvSetSavedDataAddress"},
        {8, "DbgPrint"},
        {14, "ExAllocatePoolWithTag"},
        {15, "ExFreePool"},
        {16, "ExQueryNonVolatileSetting"},
        {17, "ExSaveNonVolatileSetting"},
        {23, "FscGetCacheSize"},
        {24, "FscInvalidateIdleBlocks"},
        {40, "HalClearSoftwareInterrupt"},
        {44, "HalGetInterruptVector"},
        {46, "HalReadSMBusValue"},
        {47, "HalReadWritePCISpace"},
        {49, "HalReturnToFirmware"},
        {65, "IoCreateDevice"},
        {67, "IoCreateSymbolicLink"},
        {71, "IoDeleteSymbolicLink"},
        {74, "IoInvalidDeviceRequest"},
        {81, "IoStartNextPacket"},
        {83, "IoSynchronousDeviceIoControlRequest"},
        {87, "KeAlertThread"},
        {95, "KeBugCheck"},
        {97, "KeCancelTimer"},
        {98, "KeConnectInterrupt"},
        {99, "KeDelayExecutionThread"},
        {100, "KeDisconnectInterrupt"},
        {107, "KeInitializeDpc"},
        {109, "KeInitializeInterrupt"},
        {113, "KeInitializeTimerEx"},
        {119, "KeInsertQueueDpc"},
        {124, "KeQueryInterruptTime"},
        {125, "KeQueryPerformanceCounter"},
        {128, "KeQuerySystemTime"},
        {129, "KeRaiseIrqlToDpcLevel"},
        {137, "KeRestoreFloatingPointState"},
        {139, "KeSaveFloatingPointState"},
        {142, "KeSetBasePriorityThread"},
        {143, "KeSetDisableBoostThread"},
        {144, "KeSetEvent"},
        {145, "KeSetEventBoostPriority"},
        {149, "KeSetTimer"},
        {150, "KeSetTimerEx"},
        {151, "KeStallExecutionProcessor"},
        {153, "KeSynchronizeExecution"},
        {156, "KeTickCount"},
        {158, "KeWaitForMultipleObjects"},
        {159, "KeWaitForSingleObject"},
        {160, "KfRaiseIrql"},
        {161, "KfLowerIrql"},
        {164, "LaunchDataPage"},
        {165, "MmAllocateContiguousMemory"},
        {166, "MmAllocateContiguousMemoryEx"},
        {167, "MmAllocateSystemMemory"},
        {168, "MmClaimGpuInstanceMemory"},
        {171, "MmFreeContiguousMemory"},
        {172, "MmFreeSystemMemory"},
        {173, "MmGetPhysicalAddress"},
        {175, "MmLockUnlockBufferPages"},
        {176, "MmLockUnlockPhysicalPage"},
        {178, "MmPersistContiguousMemory"},
        {179, "MmQueryAddressProtect"},
        {180, "MmQueryAllocationSize"},
        {182, "MmSetAddressProtect"},
        {184, "NtAllocateVirtualMemory"},
        {187, "NtClose"},
        {190, "NtCreateFile"},
        {196, "NtDeviceIoControlFile"},
        {198, "NtDuplicateObject"},
        {199, "NtFlushBuffersFile"},
        {200, "NtFreeVirtualMemory"},
        {202, "NtOpenFile"},
        {203, "NtOpenSymbolicLinkObject"},
        {205, "NtQueryDirectoryFile"},
        {207, "NtQueryFullAttributesFile"},
        {210, "NtQueryInformationFile"},
        {211, "NtQuerySymbolicLinkObject"},
        {215, "NtQueryVolumeInformationFile"},
        {217, "NtReadFile"},
        {218, "NtReadFileScatter"},
        {219, "NtReleaseMutant"},
        {224, "NtSetEvent"},
        {225, "NtSetInformationFile"},
        {226, "NtSetIoCompletion"},
        {231, "NtWaitForSingleObject"},
        {232, "NtWaitForSingleObjectEx"},
        {233, "NtWaitForMultipleObjectsEx"},
        {234, "NtWriteFile"},
        {236, "NtYieldExecution"},
        {238, "ObCreateObject"},
        {246, "ObReferenceObjectByHandle"},
        {250, "ObfDereferenceObject"},
        {255, "PsCreateSystemThreadEx"},
        {258, "PsTerminateSystemThread"},
        {259, "PsThreadObjectType"},
        {269, "RtlCompareMemoryUlong"},
        {277, "RtlEnterCriticalSection"},
        {279, "RtlEqualString"},
        {289, "RtlInitAnsiString"},
        {291, "RtlInitializeCriticalSection"},
        {294, "RtlLeaveCriticalSection"},
        {301, "RtlNtStatusToDosError"},
        {302, "RtlRaiseException"},
        {304, "RtlTimeFieldsToTime"},
        {305, "RtlTimeToTimeFields"},
        {312, "RtlUnwind"},
        {322, "XboxHardwareInfo"},
        {323, "XboxHDKey"},
        {325, "XboxSignatureKey"},
        {327, "XeImageFileName"},
        {328, "XeLoadSection"},
        {335, "XcSHAInit"},
        {336, "XcSHAUpdate"},
        {337, "XcSHAFinal"},
        {354, "HalIsResetOrShutdownPending"},
        {356, "HalReadSMCTrayState"},
        {357, "MmDbgAllocateMemory"},
        {358, "MmDbgFreeMemory"},
        {359, "MmDbgQueryAvailablePages"},
        {360, "MmDbgReleaseAddress"},
    };
    return kMap;
}

} // namespace

const char* XboxKernel::OrdinalName(uint32_t ordinal) {
    const auto& m = KnownOrdinals();
    auto it = m.find(ordinal);
    return (it != m.end()) ? it->second : "UnknownOrdinal";
}

bool XboxKernel::Initialize(host::RomVfs* rom, const host::XbeImageInfo& xbe) {
    rom_ = rom;
    bound_thunks_ = 0;

    // Patch each KernelThunk table slot (0x0012A560 + i*4) with a deterministic
    // thunk dispatch token (kThunkStubBase | ordinal) for recompiled guest indirect calls.
    for (size_t i = 0; i < xbe.imported_ordinals.size(); ++i) {
        const uint32_t ord = xbe.imported_ordinals[i];
        kernel::W32(xbe.kernel_thunk_va + static_cast<uint32_t>(i * 4), kThunkStubBase | ord);
        ++bound_thunks_;
    }

    log::Info("CT3Kernel", "Bound %zu xboxkrnl.exe thunks at VA 0x%08X",
              bound_thunks_, xbe.kernel_thunk_va);
    return true;
}

void XboxKernel::Shutdown() {
    rom_ = nullptr;
    bound_thunks_ = 0;
}

uint32_t XboxKernel::DispatchThunk(uint32_t ordinal, uint32_t* guest_stack_args) {
    (void)guest_stack_args;
    switch (ordinal) {
        case 125: { // KeQueryPerformanceCounter (Xbox 733 MHz / 3.375 MHz ACPI timer)
            using namespace std::chrono;
            const auto us =
                duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
            return static_cast<uint32_t>(us);
        }
        case 156: { // KeTickCount
            using namespace std::chrono;
            return static_cast<uint32_t>(
                duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
        }
        default:
            log::Warn("CT3Kernel", "Stub thunk called: ordinal=%u (%s)",
                      ordinal, OrdinalName(ordinal));
            return 0;
    }
}

} // namespace ct2::xbox
