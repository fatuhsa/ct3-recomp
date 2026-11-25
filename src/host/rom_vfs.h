#pragma once

#include "host/xbe_loader.h"
#include "host/xiso.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ct2::host {

enum class RomFormat {
    Unknown,
    Xiso,
    Xbe,
};

struct RomInfo {
    std::string display_name;
    std::string root_dir;
    std::string image_path;
    RomFormat format = RomFormat::Unknown;
    uint64_t size_bytes = 0;
    bool using_saf_fd = false;
};

// Thread-safe Original Xbox ROM/XISO virtual filesystem:
//  1) Direct Android sdcard paths (/sdcard/crazy taxi/Crazy Taxi 3 - High Roller (USA).iso)
//  2) Android SAF detached file descriptors (Android 11-15 scoped storage)
//  3) Streams all 401 XISO files (default.xbe, Media/ADX/adx0.afs, Media/bank*.art, ...)
//     using mutex-guarded pread() so concurrent ARM64 streaming threads never interleave seeks.
class RomVfs {
public:
    RomVfs() = default;
    ~RomVfs();

    RomVfs(const RomVfs&) = delete;
    RomVfs& operator=(const RomVfs&) = delete;

    bool AutoDiscoverFromSdcard(const std::string& extra_dir = {});
    bool OpenPath(const std::string& path);
    bool OpenFd(int fd, const std::string& display_name);

    void Close();
    bool IsOpen() const;

    // Raw image pread (mutex-serialized for ARM64 multi-core safety).
    bool ReadAt(uint64_t offset, void* dst, size_t bytes, size_t* out_read = nullptr);

    // High-level Xbox disc file access (D:\default.xbe, D:\Media\ADX\adx0.afs, ...).
    bool ReadXboxFile(const std::string& xbox_path, std::vector<uint8_t>& out_data);
    bool ReadXboxFileRange(const std::string& xbox_path, uint64_t offset, void* dst, size_t bytes,
                           size_t* out_read = nullptr);
    bool LoadDefaultXbe(XbeImageInfo& out_info);

    const RomInfo& Info() const { return info_; }
    const XisoArchive& Xiso() const { return xiso_; }

    static RomFormat DetectFormat(const std::string& filename);
    static std::vector<std::string> DefaultSdcardSearchDirs();

private:
    bool TryMountXisoLocked();

    mutable std::mutex io_mu_;
    int fd_ = -1;
    RomInfo info_{};
    XisoArchive xiso_;
};

} // namespace ct2::host
