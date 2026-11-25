#include "host/xbe_loader.h"

#include "host/log.h"
#include "host/memory.h"

#include <algorithm>
#include <cstring>

namespace ct2::host {

namespace {

uint16_t U16(const std::vector<uint8_t>& b, size_t off) {
    if (off + 2 > b.size()) return 0;
    uint16_t v = 0;
    std::memcpy(&v, b.data() + off, sizeof(v));
    return v;
}

uint32_t U32(const std::vector<uint8_t>& b, size_t off) {
    if (off + 4 > b.size()) return 0;
    uint32_t v = 0;
    std::memcpy(&v, b.data() + off, sizeof(v));
    return v;
}

} // namespace

bool XbeLoader::LoadIntoGuestMemory(const std::vector<uint8_t>& xbe, XbeImageInfo& out) {
    out = XbeImageInfo{};
    if (xbe.size() < 0x180 || std::memcmp(xbe.data(), "XBEH", 4) != 0) {
        log::Error("CT3Xbe", "Invalid XBE magic or truncated header");
        return false;
    }

    out.base_addr = U32(xbe, 0x104);
    const uint32_t hdr_size = U32(xbe, 0x108);
    out.image_size = U32(xbe, 0x10C);
    const uint32_t cert_va = U32(xbe, 0x118);
    const uint32_t num_sections = U32(xbe, 0x11C);
    const uint32_t sec_hdrs_va = U32(xbe, 0x120);
    const uint32_t entry_enc = U32(xbe, 0x128);
    const uint32_t kthunk_enc = U32(xbe, 0x158);
    const uint32_t num_libs = U32(xbe, 0x160);
    const uint32_t libs_va = U32(xbe, 0x164);

    // Choose retail vs debug XOR key based on which lands inside the image VA range.
    const uint32_t entry_r = entry_enc ^ kEntryXorRetail;
    const uint32_t entry_d = entry_enc ^ kEntryXorDebug;
    out.entry_point = (entry_r >= out.base_addr && entry_r < out.base_addr + out.image_size)
                          ? entry_r
                          : entry_d;

    const uint32_t kt_r = kthunk_enc ^ kThunkXorRetail;
    const uint32_t kt_d = kthunk_enc ^ kThunkXorDebug;
    out.kernel_thunk_va = (kt_r >= out.base_addr && kt_r < out.base_addr + out.image_size)
                              ? kt_r
                              : kt_d;

    // Parse certificate metadata (TitleID, Unicode title name, region)
    if (cert_va > out.base_addr) {
        const size_t coff = cert_va - out.base_addr;
        if (coff + 0xA4 <= xbe.size()) {
            out.title_id = U32(xbe, coff + 8);
            out.game_region = U32(xbe, coff + 0xA0);
            for (size_t i = 0; i < 80; i += 2) {
                const uint16_t ch = U16(xbe, coff + 12 + i);
                if (ch == 0) break;
                out.title_name.push_back(static_cast<char>(ch & 0x7Fu));
            }
        }
    }

    // Copy XBE headers into guest memory at base_addr
    void* hdr_dst = kernel::HostPtr(out.base_addr);
    if (!hdr_dst) {
        log::Error("CT3Xbe", "Guest RAM not initialized for base 0x%08X", out.base_addr);
        return false;
    }
    const size_t copy_hdr = std::min<size_t>(hdr_size, xbe.size());
    std::memcpy(hdr_dst, xbe.data(), copy_hdr);
    kernel::MarkDirtyRange(out.base_addr, static_cast<uint32_t>(copy_hdr));

    // Map every section (zero-filling BSS tail where virtual_size > raw_size)
    if (sec_hdrs_va > out.base_addr) {
        const size_t sh_off = sec_hdrs_va - out.base_addr;
        for (uint32_t i = 0; i < num_sections; ++i) {
            const size_t eoff = sh_off + static_cast<size_t>(i) * 56;
            if (eoff + 56 > xbe.size()) break;

            XbeSectionInfo sec{};
            sec.flags = U32(xbe, eoff + 0);
            sec.virtual_addr = U32(xbe, eoff + 4);
            sec.virtual_size = U32(xbe, eoff + 8);
            sec.raw_addr = U32(xbe, eoff + 12);
            sec.raw_size = U32(xbe, eoff + 16);
            const uint32_t name_va = U32(xbe, eoff + 20);
            if (name_va >= out.base_addr && (name_va - out.base_addr) < xbe.size()) {
                const char* s = reinterpret_cast<const char*>(xbe.data() + (name_va - out.base_addr));
                sec.name.assign(s, strnlen(s, 32));
            }

            if (void* dst = kernel::HostPtr(sec.virtual_addr)) {
                std::memset(dst, 0, sec.virtual_size);
                if (sec.raw_addr < xbe.size() && sec.raw_size > 0) {
                    const size_t n = std::min<size_t>(
                        {sec.raw_size, sec.virtual_size, xbe.size() - sec.raw_addr});
                    std::memcpy(dst, xbe.data() + sec.raw_addr, n);
                }
                kernel::MarkDirtyRange(sec.virtual_addr, sec.virtual_size);
            }
            out.sections.push_back(std::move(sec));
        }
    }

    // Parse XDK library versions
    if (libs_va > out.base_addr) {
        const size_t loff = libs_va - out.base_addr;
        for (uint32_t i = 0; i < num_libs; ++i) {
            const size_t eoff = loff + static_cast<size_t>(i) * 16;
            if (eoff + 16 > xbe.size()) break;
            XbeLibraryInfo lib{};
            const char* s = reinterpret_cast<const char*>(xbe.data() + eoff);
            lib.name.assign(s, strnlen(s, 8));
            lib.major = U16(xbe, eoff + 8);
            lib.minor = U16(xbe, eoff + 10);
            lib.build = U16(xbe, eoff + 12);
            lib.flags = U16(xbe, eoff + 14);
            out.libraries.push_back(std::move(lib));
        }
    }

    // Collect imported xboxkrnl.exe ordinals from the mapped kernel thunk table
    for (uint32_t i = 0; i < 512; ++i) {
        const uint32_t raw = kernel::R32(out.kernel_thunk_va + i * 4);
        if (raw == 0) break;
        out.imported_ordinals.push_back(raw & 0x7FFFFFFFu);
    }

    log::Info("CT3Xbe",
              "Loaded XBE '%s' TitleID=0x%08X Base=0x%08X Entry=0x%08X KThunk=0x%08X (%zu imports, %zu sections)",
              out.title_name.c_str(), out.title_id, out.base_addr, out.entry_point,
              out.kernel_thunk_va, out.imported_ordinals.size(), out.sections.size());
    return true;
}

} // namespace ct2::host
