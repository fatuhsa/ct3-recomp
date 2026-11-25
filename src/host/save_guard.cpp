#include "host/save_guard.h"

#include "host/log.h"

#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace ct2::host {

namespace {

constexpr uint32_t kSaveMagic = 0x32544353u; // "SCT2"

struct SaveHeader {
    uint32_t magic;
    uint32_t payload_bytes;
    uint64_t checksum;
};

} // namespace

SaveGuard::SaveGuard(std::string save_dir) : save_dir_(std::move(save_dir)) {
    std::error_code ec;
    fs::create_directories(save_dir_, ec);
}

uint64_t SaveGuard::ComputeChecksum(const uint8_t* data, size_t len) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

bool SaveGuard::ReadVerifiedFile(const std::string& path,
                                 std::vector<uint8_t>& out_payload) const {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto sz = in.tellg();
    if (sz < static_cast<std::streamoff>(sizeof(SaveHeader))) return false;
    in.seekg(0, std::ios::beg);

    SaveHeader hdr{};
    if (!in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) return false;
    if (hdr.magic != kSaveMagic) return false;
    if (static_cast<size_t>(sz) != sizeof(SaveHeader) + hdr.payload_bytes) return false;

    out_payload.resize(hdr.payload_bytes);
    if (hdr.payload_bytes > 0 &&
        !in.read(reinterpret_cast<char*>(out_payload.data()), hdr.payload_bytes)) {
        return false;
    }
    return ComputeChecksum(out_payload.data(), out_payload.size()) == hdr.checksum;
}

void SaveGuard::RotateBackups(const std::string& slot_name, const std::string& active_path) {
    std::error_code ec;
    if (!fs::exists(active_path, ec)) return;

    const fs::path bak_dir = fs::path(save_dir_) / "backups";
    fs::create_directories(bak_dir, ec);

    for (size_t i = kMaxBackups; i >= 2; --i) {
        const fs::path older = bak_dir / (slot_name + ".bak" + std::to_string(i));
        const fs::path newer = bak_dir / (slot_name + ".bak" + std::to_string(i - 1));
        if (fs::exists(newer, ec)) {
            fs::rename(newer, older, ec);
        }
    }

    fs::copy_file(active_path, bak_dir / (slot_name + ".bak1"),
                  fs::copy_options::overwrite_existing, ec);
}

bool SaveGuard::SaveAtomic(const std::string& slot_name, const std::vector<uint8_t>& payload) {
    std::error_code ec;
    fs::create_directories(save_dir_, ec);

    const fs::path active = fs::path(save_dir_) / (slot_name + ".sav");
    const fs::path tmp = fs::path(save_dir_) / (slot_name + ".sav.tmp");

    RotateBackups(slot_name, active.string());

    SaveHeader hdr{};
    hdr.magic = kSaveMagic;
    hdr.payload_bytes = static_cast<uint32_t>(payload.size());
    hdr.checksum = ComputeChecksum(payload.data(), payload.size());

    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;

    const ssize_t w1 = ::write(fd, &hdr, sizeof(hdr));
    const ssize_t w2 = payload.empty()
                           ? 0
                           : ::write(fd, payload.data(), payload.size());
    const bool ok = (w1 == static_cast<ssize_t>(sizeof(hdr))) &&
                    (w2 == static_cast<ssize_t>(payload.size())) &&
                    (::fsync(fd) == 0);
    ::close(fd);

    if (!ok) {
        fs::remove(tmp, ec);
        return false;
    }

    fs::rename(tmp, active, ec);
    return !ec;
}

bool SaveGuard::LoadOrRestore(const std::string& slot_name, std::vector<uint8_t>& out_payload) {
    const fs::path active = fs::path(save_dir_) / (slot_name + ".sav");
    if (ReadVerifiedFile(active.string(), out_payload)) {
        return true;
    }

    const fs::path bak_dir = fs::path(save_dir_) / "backups";
    for (size_t i = 1; i <= kMaxBackups; ++i) {
        const fs::path bak = bak_dir / (slot_name + ".bak" + std::to_string(i));
        if (ReadVerifiedFile(bak.string(), out_payload)) {
            log::Warn("CT2Save", "Active save '%s' corrupted/missing; restored from backup #%zu",
                      slot_name.c_str(), i);
            std::error_code ec;
            fs::copy_file(bak, active, fs::copy_options::overwrite_existing, ec);
            return true;
        }
    }

    return false;
}

} // namespace ct2::host
