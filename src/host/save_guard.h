#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ct2::host {

// Guards VMU/save files against Android OS kills mid-write:
//  - Atomic write (.tmp -> fsync -> rename)
//  - 64-bit FNV-1a checksum envelope
//  - Rolling backup history (keeps last 10 valid saves)
//  - Automatic restore from newest verified backup if active save is truncated/corrupt
class SaveGuard {
public:
    static constexpr size_t kMaxBackups = 10;

    explicit SaveGuard(std::string save_dir);

    bool SaveAtomic(const std::string& slot_name, const std::vector<uint8_t>& payload);
    bool LoadOrRestore(const std::string& slot_name, std::vector<uint8_t>& out_payload);

    static uint64_t ComputeChecksum(const uint8_t* data, size_t len);

private:
    bool ReadVerifiedFile(const std::string& path, std::vector<uint8_t>& out_payload) const;
    void RotateBackups(const std::string& slot_name, const std::string& active_path);

    std::string save_dir_;
};

} // namespace ct2::host
