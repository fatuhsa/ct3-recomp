#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ct2::host {

struct XbeSectionInfo {
    std::string name;
    uint32_t virtual_addr = 0;
    uint32_t virtual_size = 0;
    uint32_t raw_addr = 0;
    uint32_t raw_size = 0;
    uint32_t flags = 0;
};

struct XbeLibraryInfo {
    std::string name;
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t build = 0;
    uint16_t flags = 0;
};

struct XbeImageInfo {
    uint32_t base_addr = 0;
    uint32_t image_size = 0;
    uint32_t entry_point = 0;
    uint32_t kernel_thunk_va = 0;
    uint32_t title_id = 0;
    uint32_t game_region = 0;
    std::string title_name;
    std::vector<XbeSectionInfo> sections;
    std::vector<XbeLibraryInfo> libraries;
    std::vector<uint32_t> imported_ordinals;
};

// Loads an Original Xbox XBE (e.g. Crazy Taxi 3: High Roller USA, TitleID 0x53450004, XDK 4531)
// directly into guest RAM and collects section, library, and kernel thunk ordinal metadata.
class XbeLoader {
public:
    static constexpr uint32_t kCrazyTaxi3TitleId = 0x53450004u;
    static constexpr uint32_t kEntryXorRetail = 0xA8FC57ABu;
    static constexpr uint32_t kEntryXorDebug = 0x94859D4Bu;
    static constexpr uint32_t kThunkXorRetail = 0x5B6D40B6u;
    static constexpr uint32_t kThunkXorDebug = 0xEFB1F152u;

    static bool LoadIntoGuestMemory(const std::vector<uint8_t>& xbe_bytes, XbeImageInfo& out_info);
};

} // namespace ct2::host
