#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.hpp"
#include "core/cdrom/disc_image.hpp"

namespace core::cdrom {

// On-disc structures follow Portfolio OS `discdata.h` (DiscLabel,
// DirectoryHeader, DirectoryRecord). All fields are big-endian.

struct VolumeLabel {
    u8 flags = 0;
    std::string commentary;
    std::string identifier;  // disc name
    u32 unique_id = 0;
    u32 block_size = 0;
    u32 block_count = 0;
    u32 root_unique_id = 0;
    u32 root_block_count = 0;
    u32 root_block_size = 0;
    std::vector<u32> root_avatars;  // block numbers of each copy of the root directory
};

struct DirEntry {
    static constexpr u32 kKindFile = 0x02;
    static constexpr u32 kKindSpecial = 0x06;
    static constexpr u32 kKindDirectory = 0x07;

    u32 flags = 0;
    u32 unique_id = 0;
    std::string type;  // 4-char code, e.g. "*dir", "*lbl"
    u32 block_size = 0;
    u32 byte_count = 0;
    u32 block_count = 0;
    u32 burst = 0;
    u32 gap = 0;
    std::string name;
    std::vector<u32> avatars;  // block numbers of each copy of the data

    [[nodiscard]] u32 kind() const { return flags & 0xFF; }
    [[nodiscard]] bool is_directory() const { return kind() == kKindDirectory; }
};

// Read-only Opera file system on top of a disc image. The DiscImage must
// outlive the OperaFs.
class OperaFs {
public:
    // Reads and validates the volume label at sector 0. Returns nullopt (and logs) on failure.
    static std::optional<OperaFs> mount(DiscImage& disc);

    [[nodiscard]] const VolumeLabel& label() const { return label_; }

    std::optional<std::vector<DirEntry>> read_root();
    std::optional<std::vector<DirEntry>> read_directory(const DirEntry& directory);

    // Resolves a '/'-separated path from the root. Names compare case-insensitively,
    // as in Portfolio's file folio.
    std::optional<DirEntry> find(std::string_view path);
    std::optional<DirEntry> find_launchme() { return find("LaunchMe"); }

    // Reads a file's contents from its first avatar.
    std::optional<std::vector<u8>> read_file(const DirEntry& file);

private:
    OperaFs(DiscImage& disc, VolumeLabel label) : disc_(&disc), label_(std::move(label)) {}

    std::optional<std::vector<DirEntry>> read_directory(u32 first_block, u32 block_count,
                                                        u32 block_size);

    DiscImage* disc_;
    VolumeLabel label_;
};

}  // namespace core::cdrom
