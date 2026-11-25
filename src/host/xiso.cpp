#include "host/xiso.h"

#include "host/log.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace ct2::host {

namespace {

constexpr char kXisoMagic[20] = {
    'M', 'I', 'C', 'R', 'O', 'S', 'O', 'F', 'T', '*',
    'X', 'B', 'O', 'X', '*', 'M', 'E', 'D', 'I', 'A'
};

uint16_t ReadU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (uint16_t(p[1]) << 8));
}

uint32_t ReadU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

std::string XisoArchive::NormalizeXboxPath(const std::string& path) {
    std::string s = path;
    // Strip Xbox drive prefixes ("D:\", "d:/", "\Device\CdRom0\")
    if (s.size() >= 2 && s[1] == ':') {
        s.erase(0, 2);
    }
    for (char& c : s) {
        if (c == '\\') c = '/';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (!s.empty() && s.front() == '/') s.erase(0, 1);
    return s;
}

bool XisoArchive::Mount(ReadFn reader) {
    Unmount();
    if (!reader) return false;
    reader_ = std::move(reader);

    const uint64_t candidate_bases[] = {0ull, kRedumpPartitionOffset};
    uint8_t hdr[48]{};
    uint32_t root_sector = 0;
    uint32_t root_size = 0;
    bool found = false;

    for (uint64_t base : candidate_bases) {
        size_t got = 0;
        if (!reader_(base + 32ull * kSectorSize, hdr, sizeof(hdr), &got) || got < 28) {
            continue;
        }
        if (std::memcmp(hdr, kXisoMagic, sizeof(kXisoMagic)) == 0) {
            partition_offset_ = base;
            root_sector = ReadU32LE(hdr + 20);
            root_size = ReadU32LE(hdr + 24);
            found = true;
            break;
        }
    }

    if (!found) {
        log::Warn("CT3Xiso", "MICROSOFT*XBOX*MEDIA header not found");
        return false;
    }

    if (!WalkDirectory(root_sector, root_size, "")) {
        Unmount();
        return false;
    }

    mounted_ = true;
    log::Info("CT3Xiso", "Mounted XISO (partition=0x%llx, %zu files indexed)",
              static_cast<unsigned long long>(partition_offset_), entries_.size());
    return true;
}

void XisoArchive::Unmount() {
    entries_.clear();
    partition_offset_ = 0;
    mounted_ = false;
    reader_ = nullptr;
}

bool XisoArchive::WalkDirectory(uint32_t sector, uint32_t size, const std::string& prefix) {
    if (sector == 0 || size == 0 || size > (16u << 20)) return true;

    std::vector<uint8_t> table(size);
    size_t got = 0;
    const uint64_t dir_off = partition_offset_ + static_cast<uint64_t>(sector) * kSectorSize;
    if (!reader_(dir_off, table.data(), size, &got) || got < 14) return false;

    std::vector<uint16_t> stack;
    std::unordered_set<uint16_t> visited;
    stack.push_back(0);
    visited.insert(0);

    while (!stack.empty()) {
        const uint16_t node = stack.back();
        stack.pop_back();
        const size_t off = static_cast<size_t>(node) * 4;
        if (off + 14 > got) continue;

        const uint16_t left = ReadU16LE(table.data() + off + 0);
        const uint16_t right = ReadU16LE(table.data() + off + 2);
        const uint32_t fsec = ReadU32LE(table.data() + off + 4);
        const uint32_t fsz = ReadU32LE(table.data() + off + 8);
        const uint8_t attr = table[off + 12];
        const uint8_t nlen = table[off + 13];
        if (off + 14 + nlen > got) continue;

        if (right && visited.insert(right).second) stack.push_back(right);
        if (left && visited.insert(left).second) stack.push_back(left);

        std::string name(reinterpret_cast<const char*>(table.data() + off + 14), nlen);
        std::string full = prefix.empty() ? name : (prefix + "/" + name);

        if (attr & 0x10) {
            WalkDirectory(fsec, fsz, full);
        } else {
            const std::string key = NormalizeXboxPath(full);
            XisoEntry e{};
            e.path = key;
            e.byte_offset = partition_offset_ + static_cast<uint64_t>(fsec) * kSectorSize;
            e.byte_size = fsz;
            e.is_dir = false;
            entries_.emplace(key, std::move(e));
        }
    }
    return true;
}

const XisoEntry* XisoArchive::Find(const std::string& xbox_path) const {
    const std::string key = NormalizeXboxPath(xbox_path);
    auto it = entries_.find(key);
    return (it == entries_.end()) ? nullptr : &it->second;
}

bool XisoArchive::ReadFile(const std::string& xbox_path, std::vector<uint8_t>& out_data) const {
    const XisoEntry* e = Find(xbox_path);
    if (!e) return false;
    out_data.resize(e->byte_size);
    size_t got = 0;
    return ReadFileRange(*e, 0, out_data.data(), e->byte_size, &got) && got == e->byte_size;
}

bool XisoArchive::ReadFileRange(const XisoEntry& entry, uint64_t offset, void* dst, size_t bytes,
                                size_t* out_read) const {
    if (!mounted_ || !reader_ || !dst) return false;
    if (offset >= entry.byte_size) {
        if (out_read) *out_read = 0;
        return true;
    }
    const size_t clamped =
        static_cast<size_t>(std::min<uint64_t>(bytes, entry.byte_size - offset));
    return reader_(entry.byte_offset + offset, dst, clamped, out_read);
}

} // namespace ct2::host
