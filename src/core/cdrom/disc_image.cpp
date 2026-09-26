#include "core/cdrom/disc_image.hpp"

#include <algorithm>
#include <fstream>

#include "common/log.hpp"

namespace core::cdrom {

namespace {

constexpr u32 kRawSectorSize = 2352;
constexpr std::array<u8, 12> kSyncPattern = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                             0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
constexpr std::array<u8, 8> kChdMagic = {'M', 'C', 'o', 'm', 'p', 'r', 'H', 'D'};

struct Layout {
    u32 stride;
    u32 data_offset;
};

constexpr Layout layout_of(DiscImage::Format format) {
    switch (format) {
        case DiscImage::Format::Raw2352Mode1: return {kRawSectorSize, 16};
        case DiscImage::Format::Raw2352Mode2Form1: return {kRawSectorSize, 24};
        default: return {kSectorSize, 0};
    }
}

// Uncompressed image: a plain file of 2048- or 2352-byte sectors.
class FileDiscImage final : public DiscImage {
public:
    FileDiscImage(std::ifstream file, Format format, u32 sector_count)
        : file_(std::move(file)),
          format_(format),
          stride_(layout_of(format).stride),
          data_offset_(layout_of(format).data_offset),
          sector_count_(sector_count) {}

    bool read_sector(u32 lba, std::span<u8, kSectorSize> out) override;
    [[nodiscard]] u32 sector_count() const override { return sector_count_; }
    [[nodiscard]] Format format() const override { return format_; }

private:
    std::ifstream file_;
    Format format_;
    u32 stride_;
    u32 data_offset_;
    u32 sector_count_;
};

}  // namespace

std::unique_ptr<DiscImage> DiscImage::open(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Log::error("Cannot open disc image '{}'", path.string());
        return nullptr;
    }

    std::array<u8, 16> head{};
    file.read(reinterpret_cast<char*>(head.data()), head.size());
    if (file.gcount() != static_cast<std::streamsize>(head.size())) {
        Log::error("Disc image '{}' is too small", path.string());
        return nullptr;
    }
    if (std::ranges::equal(std::span(head).first<8>(), kChdMagic)) return open_chd_image(path);

    // Raw sectors start with the CD sync pattern; byte 15 is the sector mode.
    Format format = Format::Cooked2048;
    if (std::ranges::equal(std::span(head).first<12>(), kSyncPattern)) {
        switch (head[15]) {
            case 1: format = Format::Raw2352Mode1; break;
            case 2: format = Format::Raw2352Mode2Form1; break;
            default:
                Log::error("Disc image '{}': unsupported raw sector mode {}", path.string(), head[15]);
                return nullptr;
        }
    }

    const auto file_size = std::filesystem::file_size(path);
    const Layout layout = layout_of(format);
    if (file_size % layout.stride != 0)
        Log::warn("Disc image '{}': size {} is not a multiple of {}; ignoring trailing bytes",
                  path.string(), file_size, layout.stride);

    file.clear();
    return std::make_unique<FileDiscImage>(std::move(file), format,
                                           static_cast<u32>(file_size / layout.stride));
}

bool FileDiscImage::read_sector(u32 lba, std::span<u8, kSectorSize> out) {
    if (lba >= sector_count_) {
        Log::warn("Disc read past end: sector {} of {}", lba, sector_count_);
        return false;
    }
    const auto position = static_cast<std::streamoff>(u64{lba} * stride_ + data_offset_);
    file_.seekg(position);
    file_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (file_.gcount() != static_cast<std::streamsize>(out.size())) {
        Log::error("Disc read failed at sector {}", lba);
        file_.clear();
        return false;
    }
    return true;
}

std::string_view DiscImage::format_name(Format format) {
    switch (format) {
        case Format::Cooked2048: return "ISO (2048 bytes/sector)";
        case Format::Raw2352Mode1: return "raw BIN (2352 bytes/sector, Mode 1)";
        case Format::Raw2352Mode2Form1: return "raw BIN (2352 bytes/sector, Mode 2 Form 1)";
        case Format::Chd: return "CHD (compressed)";
    }
    return "unknown";
}

}  // namespace core::cdrom
