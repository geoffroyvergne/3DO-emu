// Tests the Opera FS parser against synthetic disc images built to the
// Portfolio OS discdata.h layout.
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

#include "common/endian.hpp"
#include "core/cdrom/disc_image.hpp"
#include "core/cdrom/opera_fs.hpp"
#include "test_common.hpp"

namespace {

using core::cdrom::kSectorSize;
using core::cdrom::DiscImage;
using core::cdrom::OperaFs;

constexpr u32 kLastInBlock = 0x40000000;
constexpr u32 kLastInDir = 0x80000000;
constexpr u32 kSectorCount = 16;

// Synthetic image layout (block numbers):
//   0      volume label, root = 2 blocks at 2 (copy at 6)
//   2      root block 0: LaunchMe, data/            -> next block 1
//   3      root block 1: readme.txt
//   5      data/ block 0: level1.dat
//   10-11  LaunchMe contents (3000 bytes)
//   12     level1.dat contents
class ImageBuilder {
public:
    ImageBuilder() : data_(kSectorCount * kSectorSize) {}

    void u32_at(u32 block, std::size_t offset, u32 value) {
        store_be32(&data_[block * kSectorSize + offset], value);
    }
    void text_at(u32 block, std::size_t offset, std::string_view text) {
        std::memcpy(&data_[block * kSectorSize + offset], text.data(), text.size());
    }

    void label() {
        data_[0] = 1;
        std::memset(&data_[1], 0x5A, 5);
        data_[6] = 1;
        text_at(0, 8, "test commentary");
        text_at(0, 40, "TESTDISC");
        u32_at(0, 72, 0x12345678);
        u32_at(0, 76, kSectorSize);
        u32_at(0, 80, kSectorCount);
        u32_at(0, 84, 0xABCD);
        u32_at(0, 88, 2);
        u32_at(0, 92, kSectorSize);
        u32_at(0, 96, 1);   // last avatar index -> 2 copies
        u32_at(0, 100, 2);
        u32_at(0, 104, 6);
    }

    void dir_header(u32 block, s32 next, s32 prev) {
        u32_at(block, 0, static_cast<u32>(next));
        u32_at(block, 4, static_cast<u32>(prev));
        u32_at(block, 16, 20);
    }

    // Returns the offset just past the record.
    std::size_t record(u32 block, std::size_t offset, u32 flags, std::string_view type,
                       std::string_view name, u32 bytes, u32 blocks, std::vector<u32> avatars) {
        u32_at(block, offset + 0, flags);
        u32_at(block, offset + 4, 0x1000 + static_cast<u32>(offset));
        text_at(block, offset + 8, type);
        u32_at(block, offset + 12, kSectorSize);
        u32_at(block, offset + 16, bytes);
        u32_at(block, offset + 20, blocks);
        text_at(block, offset + 32, name);
        u32_at(block, offset + 64, static_cast<u32>(avatars.size() - 1));
        for (std::size_t i = 0; i < avatars.size(); ++i)
            u32_at(block, offset + 68 + 4 * i, avatars[i]);
        return offset + 68 + 4 * avatars.size();
    }

    void build_standard() {
        label();
        dir_header(2, 1, -1);
        std::size_t off = record(2, 20, 0x02, "EXEC", "LaunchMe", 3000, 2, {10, 13});
        record(2, off, 0x07 | kLastInBlock, "*dir", "data", kSectorSize, 1, {5});
        dir_header(3, -1, 0);
        record(3, 20, 0x02 | kLastInBlock | kLastInDir, "TEXT", "readme.txt", 5, 1, {12});
        dir_header(5, -1, -1);
        record(5, 20, 0x02 | kLastInBlock | kLastInDir, "DATA", "level1.dat", 4, 1, {12});
        for (u32 i = 0; i < 3000; ++i) data_[10 * kSectorSize + i] = static_cast<u8>(i * 7);
        text_at(12, 0, "LVL1");
    }

    void write_cooked(const std::filesystem::path& path) const {
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(data_.data()), static_cast<std::streamsize>(data_.size()));
    }

    void write_raw_mode1(const std::filesystem::path& path) const {
        std::ofstream file(path, std::ios::binary);
        std::vector<u8> raw(2352);
        for (u32 s = 0; s < kSectorCount; ++s) {
            std::fill(raw.begin(), raw.end(), u8{0});
            std::fill(raw.begin() + 1, raw.begin() + 11, u8{0xFF});
            raw[15] = 1;  // mode 1
            std::memcpy(&raw[16], &data_[s * kSectorSize], kSectorSize);
            file.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
        }
    }

    std::vector<u8> data_;
};

std::filesystem::path temp_path(std::string_view name) {
    return std::filesystem::temp_directory_path() / name;
}

void check_filesystem(const std::filesystem::path& path, DiscImage::Format expected_format) {
    auto disc = DiscImage::open(path);
    CHECK_EQ(disc != nullptr, true);
    if (!disc) return;
    CHECK_EQ(static_cast<int>(disc->format()), static_cast<int>(expected_format));
    CHECK_EQ(disc->sector_count(), kSectorCount);

    auto fs = OperaFs::mount(*disc);
    CHECK_EQ(fs.has_value(), true);
    if (!fs) return;
    CHECK_EQ(fs->label().identifier == "TESTDISC", true);
    CHECK_EQ(fs->label().commentary == "test commentary", true);
    CHECK_EQ(fs->label().root_avatars.size(), 2u);
    CHECK_EQ(fs->label().root_avatars[1], 6u);

    // Root spans two blocks, linked by dh_NextBlock.
    const auto root = fs->read_root();
    CHECK_EQ(root.has_value(), true);
    if (!root) return;
    CHECK_EQ(root->size(), 3u);
    if (root->size() != 3) return;
    CHECK_EQ((*root)[0].name == "LaunchMe", true);
    CHECK_EQ((*root)[0].type == "EXEC", true);
    CHECK_EQ((*root)[0].avatars.size(), 2u);        // variable-length record
    CHECK_EQ((*root)[1].name == "data", true);
    CHECK_EQ((*root)[1].is_directory(), true);
    CHECK_EQ((*root)[2].name == "readme.txt", true);  // from the second root block

    const auto launchme = fs->find_launchme();
    CHECK_EQ(launchme.has_value(), true);
    CHECK_EQ(fs->find("/LAUNCHME").has_value(), true);  // case-insensitive
    CHECK_EQ(fs->find("missing").has_value(), false);
    CHECK_EQ(fs->find("LaunchMe/x").has_value(), false);  // not a directory

    const auto nested = fs->find("/data/level1.dat");
    CHECK_EQ(nested.has_value(), true);
    if (nested) {
        const auto contents = fs->read_file(*nested);
        CHECK_EQ(contents.has_value() && contents->size() == 4 && (*contents)[0] == 'L', true);
    }

    if (launchme) {
        const auto exe = fs->read_file(*launchme);
        CHECK_EQ(exe.has_value(), true);
        if (exe) {
            CHECK_EQ(exe->size(), 3000u);
            CHECK_EQ((*exe)[2047], static_cast<u8>(2047 * 7));
            CHECK_EQ((*exe)[2999], static_cast<u8>(2999 * 7));  // crosses into 2nd block
        }
    }
}

void test_cooked_image() {
    ImageBuilder image;
    image.build_standard();
    const auto path = temp_path("opera_fs_test.iso");
    image.write_cooked(path);
    check_filesystem(path, DiscImage::Format::Cooked2048);
    std::filesystem::remove(path);
}

void test_raw_image() {
    ImageBuilder image;
    image.build_standard();
    const auto path = temp_path("opera_fs_test.bin");
    image.write_raw_mode1(path);
    check_filesystem(path, DiscImage::Format::Raw2352Mode1);
    std::filesystem::remove(path);
}

void test_rejects_non_opera_image() {
    ImageBuilder image;  // all zeros: no label
    const auto path = temp_path("opera_fs_test_blank.iso");
    image.write_cooked(path);
    auto disc = DiscImage::open(path);
    CHECK_EQ(disc != nullptr, true);
    if (disc) CHECK_EQ(OperaFs::mount(*disc).has_value(), false);
    std::filesystem::remove(path);
}

void test_rejects_directory_loop() {
    ImageBuilder image;
    image.build_standard();
    image.dir_header(3, 0, 0);  // root block 1 links back to block 0
    image.record(3, 20, 0x02 | kLastInBlock, "TEXT", "readme.txt", 5, 1, {12});
    const auto path = temp_path("opera_fs_test_loop.iso");
    image.write_cooked(path);
    auto disc = DiscImage::open(path);
    if (disc) {
        auto fs = OperaFs::mount(*disc);
        if (fs) CHECK_EQ(fs->read_root().has_value(), false);
    }
    std::filesystem::remove(path);
}

}  // namespace

int main() {
    test_cooked_image();
    test_raw_image();
    test_rejects_non_opera_image();
    test_rejects_directory_loop();
    return report("opera_fs_test");
}
