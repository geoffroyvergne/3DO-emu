#pragma once

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string_view>

#include "common/types.hpp"

namespace core::cdrom {

inline constexpr std::size_t kSectorSize = 2048;  // user data per CD-ROM sector
using Sector = std::array<u8, kSectorSize>;

// Read-only access to the data track of a disc image, one 2048-byte sector at a time.
// Handles cooked images (.iso, 2048 bytes/sector) and raw images (.bin, 2352 bytes/sector).
class DiscImage {
public:
    enum class Format {
        Cooked2048,         // .iso: user data only
        Raw2352Mode1,       // .bin: sync + header + 2048 data + EDC/ECC
        Raw2352Mode2Form1,  // .bin: sync + header + subheader + 2048 data + EDC/ECC
    };

    // Returns nullptr (and logs why) if the file cannot be opened or recognised.
    static std::unique_ptr<DiscImage> open(const std::filesystem::path& path);

    bool read_sector(u32 lba, std::span<u8, kSectorSize> out);

    [[nodiscard]] u32 sector_count() const { return sector_count_; }
    [[nodiscard]] Format format() const { return format_; }
    static std::string_view format_name(Format format);

private:
    DiscImage(std::ifstream file, Format format, u32 sector_count);

    std::ifstream file_;
    Format format_;
    u32 stride_;       // bytes per sector in the file
    u32 data_offset_;  // offset of the 2048 user bytes inside a sector
    u32 sector_count_;
};

}  // namespace core::cdrom
