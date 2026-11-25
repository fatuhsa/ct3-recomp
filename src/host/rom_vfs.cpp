#include "host/rom_vfs.h"

#include "host/log.h"

#include <algorithm>
#include <cctype>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace ct2::host {

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

int FormatPriority(RomFormat fmt) {
    switch (fmt) {
        case RomFormat::Xiso: return 100;
        case RomFormat::Xbe:  return 90;
        default:              return 0;
    }
}

} // namespace

RomVfs::~RomVfs() {
    Close();
}

std::vector<std::string> RomVfs::DefaultSdcardSearchDirs() {
    return {
        "/sdcard/crazy taxi",
        "/storage/emulated/0/crazy taxi",
        "/sdcard/Crazy Taxi",
        "/storage/emulated/0/Crazy Taxi",
        "/sdcard/crazy_taxi",
        "/sdcard/ct3",
        "/sdcard/Android/data/com.ct2recomp.android/files/rom",
    };
}

RomFormat RomVfs::DetectFormat(const std::string& filename) {
    const std::string lower = ToLower(fs::path(filename).extension().string());
    if (lower == ".iso" || lower == ".xiso") return RomFormat::Xiso;
    if (lower == ".xbe") return RomFormat::Xbe;
    return RomFormat::Unknown;
}

bool RomVfs::AutoDiscoverFromSdcard(const std::string& extra_dir) {
    std::vector<std::string> dirs;
    if (!extra_dir.empty()) dirs.push_back(extra_dir);
    auto defaults = DefaultSdcardSearchDirs();
    dirs.insert(dirs.end(), defaults.begin(), defaults.end());

    std::string best_path;
    int best_score = 0;

    for (const auto& dir : dirs) {
        std::error_code ec;
        if (!fs::exists(dir, ec)) continue;
        if (fs::is_regular_file(dir, ec)) {
            if (DetectFormat(dir) != RomFormat::Unknown) return OpenPath(dir);
            continue;
        }
        if (!fs::is_directory(dir, ec)) continue;

        for (const auto& entry : fs::recursive_directory_iterator(
                 dir, fs::directory_options::skip_permission_denied, ec)) {
            if (ec || !entry.is_regular_file(ec)) continue;
            const std::string path = entry.path().string();
            const RomFormat fmt = DetectFormat(path);
            int score = FormatPriority(fmt);
            if (score == 0) continue;

            const std::string lower_name = ToLower(entry.path().filename().string());
            if (lower_name.find("taxi 3") != std::string::npos ||
                lower_name.find("taxi3") != std::string::npos ||
                lower_name.find("high roller") != std::string::npos ||
                lower_name == "default.xbe") {
                score += 60;
            }

            if (score > best_score) {
                best_score = score;
                best_path = path;
            }
        }
    }

    if (best_path.empty()) {
        log::Warn("CT3Rom", "No Crazy Taxi 3 ISO/XBE found under /sdcard/crazy taxi");
        return false;
    }
    return OpenPath(best_path);
}

bool RomVfs::TryMountXisoLocked() {
    xiso_.Unmount();
    if (info_.format != RomFormat::Xiso) return true;

    auto reader = [this](uint64_t offset, void* dst, size_t bytes, size_t* out_read) -> bool {
        size_t total = 0;
        auto* out = static_cast<uint8_t*>(dst);
        while (total < bytes) {
            const ssize_t n = ::pread(fd_, out + total, bytes - total,
                                      static_cast<off_t>(offset + total));
            if (n <= 0) break;
            total += static_cast<size_t>(n);
        }
        if (out_read) *out_read = total;
        return total > 0 || bytes == 0;
    };
    return xiso_.Mount(reader);
}

bool RomVfs::OpenPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(io_mu_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        log::Error("CT3Rom", "Failed to open ROM path: %s", path.c_str());
        return false;
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return false;
    }

    fd_ = fd;
    const fs::path p(path);
    info_.display_name = p.filename().string();
    info_.root_dir = p.parent_path().string();
    info_.image_path = path;
    info_.format = DetectFormat(info_.display_name);
    info_.size_bytes = static_cast<uint64_t>(st.st_size);
    info_.using_saf_fd = false;

    if (!TryMountXisoLocked()) {
        log::Warn("CT3Rom", "Opened %s (%llu MB) but XISO header was not detected",
                  path.c_str(), static_cast<unsigned long long>(info_.size_bytes >> 20));
    } else {
        log::Info("CT3Rom", "Opened ROM: %s (%llu MB, XISO files=%zu)",
                  path.c_str(), static_cast<unsigned long long>(info_.size_bytes >> 20),
                  xiso_.FileCount());
    }
    return true;
}

bool RomVfs::OpenFd(int fd, const std::string& display_name) {
    std::lock_guard<std::mutex> lock(io_mu_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (fd < 0) return false;

    struct stat st{};
    uint64_t sz = 0;
    if (::fstat(fd, &st) == 0 && st.st_size > 0) sz = static_cast<uint64_t>(st.st_size);

    fd_ = fd;
    info_.display_name = display_name;
    info_.root_dir.clear();
    info_.image_path = "saf://" + display_name;
    info_.format = DetectFormat(display_name);
    info_.size_bytes = sz;
    info_.using_saf_fd = true;

    TryMountXisoLocked();
    log::Info("CT3Rom", "Opened SAF ROM fd=%d (%s, %llu MB, XISO files=%zu)",
              fd_, display_name.c_str(), static_cast<unsigned long long>(sz >> 20),
              xiso_.FileCount());
    return true;
}

void RomVfs::Close() {
    std::lock_guard<std::mutex> lock(io_mu_);
    xiso_.Unmount();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    info_ = RomInfo{};
}

bool RomVfs::IsOpen() const {
    std::lock_guard<std::mutex> lock(io_mu_);
    return fd_ >= 0;
}

bool RomVfs::ReadAt(uint64_t offset, void* dst, size_t bytes, size_t* out_read) {
    std::lock_guard<std::mutex> lock(io_mu_);
    if (fd_ < 0 || !dst) return false;

    size_t total = 0;
    auto* out = static_cast<uint8_t*>(dst);
    while (total < bytes) {
        const ssize_t n = ::pread(fd_, out + total, bytes - total,
                                  static_cast<off_t>(offset + total));
        if (n <= 0) break;
        total += static_cast<size_t>(n);
    }
    if (out_read) *out_read = total;
    return total > 0 || bytes == 0;
}

bool RomVfs::ReadXboxFile(const std::string& xbox_path, std::vector<uint8_t>& out_data) {
    std::lock_guard<std::mutex> lock(io_mu_);
    if (fd_ < 0) return false;

    if (xiso_.IsMounted()) {
        return xiso_.ReadFile(xbox_path, out_data);
    }

    // Extracted directory fallback (when user opened default.xbe directly)
    const std::string norm = XisoArchive::NormalizeXboxPath(xbox_path);
    if (norm == "default.xbe" && info_.format == RomFormat::Xbe) {
        out_data.resize(static_cast<size_t>(info_.size_bytes));
        size_t total = 0;
        while (total < out_data.size()) {
            const ssize_t n = ::pread(fd_, out_data.data() + total,
                                      out_data.size() - total, static_cast<off_t>(total));
            if (n <= 0) break;
            total += static_cast<size_t>(n);
        }
        return total == out_data.size();
    }

    if (info_.root_dir.empty()) return false;
    const fs::path full = fs::path(info_.root_dir) / norm;
    std::ifstream in(full, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto sz = in.tellg();
    if (sz < 0) return false;
    out_data.resize(static_cast<size_t>(sz));
    in.seekg(0, std::ios::beg);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(out_data.data()), sz));
}

bool RomVfs::ReadXboxFileRange(const std::string& xbox_path, uint64_t offset, void* dst,
                               size_t bytes, size_t* out_read) {
    std::lock_guard<std::mutex> lock(io_mu_);
    if (!xiso_.IsMounted()) return false;
    const XisoEntry* e = xiso_.Find(xbox_path);
    if (!e) return false;
    return xiso_.ReadFileRange(*e, offset, dst, bytes, out_read);
}

bool RomVfs::LoadDefaultXbe(XbeImageInfo& out_info) {
    std::vector<uint8_t> xbe_bytes;
    if (!ReadXboxFile("default.xbe", xbe_bytes)) {
        log::Error("CT3Rom", "Failed to read default.xbe from mounted ROM");
        return false;
    }
    return XbeLoader::LoadIntoGuestMemory(xbe_bytes, out_info);
}

} // namespace ct2::host
