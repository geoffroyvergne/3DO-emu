#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

#include "common/types.hpp"

namespace core::cdrom {

inline constexpr std::size_t kSectorSize = 2048;  // user data per CD-ROM sector
using Sector = std::array<u8, kSectorSize>;

// Read-only access to the data track of a disc image, one 2048-byte sector at a time.
class DiscImage {
public:
    enum class Format {
        Cooked2048,         // .iso: user data only
        Raw2352Mode1,       // .bin: sync + header + 2048 data + EDC/ECC
        Raw2352Mode2Form1,  // .bin: sync + header + subheader + 2048 data + EDC/ECC
        Chd,                // MAME compressed hunks of raw CD frames
    };

    virtual ~DiscImage() = default;

    // Opens .iso, raw .bin or .chd (recognised by content, not extension).
    // Returns nullptr (and logs why) if the file cannot be opened or recognised.
    static std::unique_ptr<DiscImage> open(const std::filesystem::path& path);

    virtual bool read_sector(u32 lba, std::span<u8, kSectorSize> out) = 0;

    [[nodiscard]] virtual u32 sector_count() const = 0;
    [[nodiscard]] virtual Format format() const = 0;
    static std::string_view format_name(Format format);
};

// Implemented in chd_image.cpp.
std::unique_ptr<DiscImage> open_chd_image(const std::filesystem::path& path);

}  // namespace core::cdrom
