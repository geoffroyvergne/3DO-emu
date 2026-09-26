#include "core/cdrom/opera_fs.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "common/endian.hpp"
#include "common/log.hpp"

namespace core::cdrom {

namespace {

// DiscLabel layout.
constexpr std::size_t kLabelRecordType = 0;
constexpr std::size_t kLabelSync = 1;
constexpr std::size_t kLabelVersion = 6;
constexpr std::size_t kLabelFlags = 7;
constexpr std::size_t kLabelCommentary = 8;
constexpr std::size_t kLabelIdentifier = 40;
constexpr std::size_t kLabelUniqueId = 72;
constexpr std::size_t kLabelBlockSize = 76;
constexpr std::size_t kLabelBlockCount = 80;
constexpr std::size_t kLabelRootUniqueId = 84;
constexpr std::size_t kLabelRootBlockCount = 88;
constexpr std::size_t kLabelRootBlockSize = 92;
constexpr std::size_t kLabelRootLastAvatar = 96;
constexpr std::size_t kLabelRootAvatars = 100;
constexpr u32 kRootHighestAvatar = 7;
constexpr u8 kVolumeSyncByte = 0x5A;
constexpr std::size_t kVolumeSyncLen = 5;
constexpr std::size_t kTextLen = 32;

// DirectoryHeader layout.
constexpr std::size_t kHeaderNextBlock = 0;
constexpr std::size_t kHeaderFirstEntryOffset = 16;

// DirectoryRecord layout.
constexpr std::size_t kRecordFlags = 0;
constexpr std::size_t kRecordUniqueId = 4;
constexpr std::size_t kRecordType = 8;
constexpr std::size_t kRecordBlockSize = 12;
constexpr std::size_t kRecordByteCount = 16;
constexpr std::size_t kRecordBlockCount = 20;
constexpr std::size_t kRecordBurst = 24;
constexpr std::size_t kRecordGap = 28;
constexpr std::size_t kRecordName = 32;
constexpr std::size_t kRecordLastAvatar = 64;
constexpr std::size_t kRecordAvatars = 68;

constexpr u32 kLastInBlock = 0x40000000;
constexpr u32 kLastInDir = 0x80000000;

// Largest file read_file will load; a full disc is ~650 MB, a single
// executable or asset is far smaller.
constexpr u64 kMaxFileBytes = 64ull * 1024 * 1024;

u32 be32_at(const Sector& sector, std::size_t offset) {
    return load_be32(sector.data() + offset);
}

// Null-terminated, fixed-width ASCII field.
std::string text_at(const Sector& sector, std::size_t offset, std::size_t length) {
    const char* begin = reinterpret_cast<const char*>(sector.data() + offset);
    return std::string(begin, strnlen(begin, length));
}

std::string fourcc_at(const Sector& sector, std::size_t offset) {
    std::string code(reinterpret_cast<const char*>(sector.data() + offset), 4);
    for (char& c : code)
        if (!std::isprint(static_cast<unsigned char>(c))) c = '.';
    return code;
}

bool iequals(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

}  // namespace

std::optional<OperaFs> OperaFs::mount(DiscImage& disc) {
    Sector sector{};
    if (!disc.read_sector(0, sector)) return std::nullopt;

    const bool sync_ok = std::all_of(sector.begin() + kLabelSync,
                                     sector.begin() + kLabelSync + kVolumeSyncLen,
                                     [](u8 b) { return b == kVolumeSyncByte; });
    if (sector[kLabelRecordType] != 1 || !sync_ok) {
        Log::error("Not an Opera volume: no disc label at sector 0");
        return std::nullopt;
    }
    if (sector[kLabelVersion] != 1)
        Log::warn("Opera label: unexpected structure version {}", sector[kLabelVersion]);

    VolumeLabel label;
    label.flags = sector[kLabelFlags];
    label.commentary = text_at(sector, kLabelCommentary, kTextLen);
    label.identifier = text_at(sector, kLabelIdentifier, kTextLen);
    label.unique_id = be32_at(sector, kLabelUniqueId);
    label.block_size = be32_at(sector, kLabelBlockSize);
    label.block_count = be32_at(sector, kLabelBlockCount);
    label.root_unique_id = be32_at(sector, kLabelRootUniqueId);
    label.root_block_count = be32_at(sector, kLabelRootBlockCount);
    label.root_block_size = be32_at(sector, kLabelRootBlockSize);

    if (label.block_size != kSectorSize || label.root_block_size != kSectorSize) {
        Log::error("Opera label: unsupported block size {} (root {})", label.block_size,
                   label.root_block_size);
        return std::nullopt;
    }

    const u32 last_avatar = be32_at(sector, kLabelRootLastAvatar);
    if (last_avatar > kRootHighestAvatar) {
        Log::error("Opera label: root avatar index {} exceeds {}", last_avatar, kRootHighestAvatar);
        return std::nullopt;
    }
    for (u32 i = 0; i <= last_avatar; ++i)
        label.root_avatars.push_back(be32_at(sector, kLabelRootAvatars + 4 * i));

    if (label.block_count != disc.sector_count())
        Log::warn("Opera label: {} blocks declared, image has {}", label.block_count,
                  disc.sector_count());

    return OperaFs(disc, std::move(label));
}

std::optional<std::vector<DirEntry>> OperaFs::read_root() {
    return read_directory(label_.root_avatars.front(), label_.root_block_count,
                          label_.root_block_size);
}

std::optional<std::vector<DirEntry>> OperaFs::read_directory(const DirEntry& directory) {
    if (!directory.is_directory() || directory.avatars.empty()) {
        Log::warn("'{}' is not a directory", directory.name);
        return std::nullopt;
    }
    return read_directory(directory.avatars.front(), directory.block_count, directory.block_size);
}

std::optional<std::vector<DirEntry>> OperaFs::read_directory(u32 first_block, u32 block_count,
                                                             u32 block_size) {
    if (block_size != kSectorSize) {
        Log::error("Directory at block {}: unsupported block size {}", first_block, block_size);
        return std::nullopt;
    }

    std::vector<DirEntry> entries;
    Sector sector{};
    s32 block = 0;  // relative to the start of the directory
    u32 blocks_visited = 0;

    while (block >= 0) {
        if (static_cast<u32>(block) >= block_count || ++blocks_visited > block_count) {
            Log::error("Directory at block {}: bad block link {}", first_block, block);
            return std::nullopt;
        }
        if (!disc_->read_sector(first_block + static_cast<u32>(block), sector)) return std::nullopt;

        const auto next_block = static_cast<s32>(be32_at(sector, kHeaderNextBlock));
        std::size_t offset = be32_at(sector, kHeaderFirstEntryOffset);

        for (;;) {
            if (offset + kRecordAvatars > kSectorSize) {
                Log::error("Directory at block {}: entry overruns block", first_block);
                return std::nullopt;
            }
            const u32 last_avatar = be32_at(sector, offset + kRecordLastAvatar);
            const std::size_t record_size = kRecordAvatars + 4 * (std::size_t{last_avatar} + 1);
            if (offset + record_size > kSectorSize) {
                Log::error("Directory at block {}: avatar list overruns block", first_block);
                return std::nullopt;
            }

            DirEntry& entry = entries.emplace_back();
            entry.flags = be32_at(sector, offset + kRecordFlags);
            entry.unique_id = be32_at(sector, offset + kRecordUniqueId);
            entry.type = fourcc_at(sector, offset + kRecordType);
            entry.block_size = be32_at(sector, offset + kRecordBlockSize);
            entry.byte_count = be32_at(sector, offset + kRecordByteCount);
            entry.block_count = be32_at(sector, offset + kRecordBlockCount);
            entry.burst = be32_at(sector, offset + kRecordBurst);
            entry.gap = be32_at(sector, offset + kRecordGap);
            entry.name = text_at(sector, offset + kRecordName, kTextLen);
            for (u32 i = 0; i <= last_avatar; ++i)
                entry.avatars.push_back(be32_at(sector, offset + kRecordAvatars + 4 * i));

            if (entry.flags & kLastInDir) return entries;
            if (entry.flags & kLastInBlock) break;
            offset += record_size;
        }
        block = next_block;
    }
    return entries;
}

std::optional<DirEntry> OperaFs::find(std::string_view path) {
    auto entries = read_root();
    std::optional<DirEntry> current;

    while (!path.empty()) {
        const std::size_t slash = path.find('/');
        const std::string_view component = path.substr(0, slash);
        path = (slash == std::string_view::npos) ? std::string_view{} : path.substr(slash + 1);
        if (component.empty()) continue;

        if (current) {
            if (!current->is_directory()) return std::nullopt;
            entries = read_directory(*current);
        }
        if (!entries) return std::nullopt;

        const auto it = std::ranges::find_if(
            *entries, [&](const DirEntry& e) { return iequals(e.name, component); });
        if (it == entries->end()) return std::nullopt;
        current = *it;
    }
    return current;
}

std::optional<std::vector<u8>> OperaFs::read_file(const DirEntry& file) {
    if (file.is_directory() || file.avatars.empty()) {
        Log::warn("'{}' is not a readable file", file.name);
        return std::nullopt;
    }
    if (file.block_size != kSectorSize || file.byte_count > kMaxFileBytes ||
        u64{file.block_count} * kSectorSize < file.byte_count) {
        Log::error("'{}': inconsistent size ({} bytes, {} blocks of {})", file.name,
                   file.byte_count, file.block_count, file.block_size);
        return std::nullopt;
    }

    std::vector<u8> data(u64{file.block_count} * kSectorSize);
    for (u32 i = 0; i < file.block_count; ++i) {
        auto out = std::span<u8, kSectorSize>(data.data() + std::size_t{i} * kSectorSize, kSectorSize);
        if (!disc_->read_sector(file.avatars.front() + i, out)) return std::nullopt;
    }
    data.resize(file.byte_count);
    return data;
}

}  // namespace core::cdrom
