#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ct2::host {

struct XisoEntry {
    std::string path;   // Normalized lowercase forward-slash path (e.g. "media/adx/adx0.afs")
    uint64_t byte_offset = 0;
    uint32_t byte_size = 0;
    bool is_dir = false;
};

// Streaming Original Xbox XISO ("MICROSOFT*XBOX*MEDIA") parser.
// Supports both trimmed XISO (partition offset 0) and full Redump DVD dumps (offset 0x18300000).
class XisoArchive {
public:
    static constexpr uint32_t kSectorSize = 2048;
    static constexpr uint64_t kRedumpPartitionOffset = 0x18300000ull;

    using ReadFn = std::function<bool(uint64_t offset, void* dst, size_t bytes, size_t* out_read)>;

    bool Mount(ReadFn reader);
    void Unmount();

    bool IsMounted() const { return mounted_; }
    uint64_t PartitionOffset() const { return partition_offset_; }
    size_t FileCount() const { return entries_.size(); }

    // Lookup Xbox path (accepts "D:\\Media\\ADX\\adx0.afs", "media/adx/adx0.afs", "default.xbe").
    const XisoEntry* Find(const std::string& xbox_path) const;

    bool ReadFile(const std::string& xbox_path, std::vector<uint8_t>& out_data) const;
    bool ReadFileRange(const XisoEntry& entry, uint64_t offset, void* dst, size_t bytes,
                       size_t* out_read = nullptr) const;

    static std::string NormalizeXboxPath(const std::string& path);

private:
    bool WalkDirectory(uint32_t sector, uint32_t size, const std::string& prefix);

    ReadFn reader_;
    uint64_t partition_offset_ = 0;
    bool mounted_ = false;
    std::unordered_map<std::string, XisoEntry> entries_;
};

} // namespace ct2::host
