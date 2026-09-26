// opera-ls: prints the volume label and a directory listing of a 3DO disc image.
//
//   usage: opera-ls <image.iso|image.bin> [directory path]
#include <cstdio>
#include <format>
#include <string>

#include "core/cdrom/disc_image.hpp"
#include "core/cdrom/opera_fs.hpp"

namespace {

using core::cdrom::DirEntry;

template <typename... Args>
void out(std::format_string<Args...> fmt, Args&&... args) {
    std::fputs(std::format(fmt, std::forward<Args>(args)...).c_str(), stdout);
}

std::string_view kind_name(const DirEntry& entry) {
    switch (entry.kind()) {
        case DirEntry::kKindFile: return "file";
        case DirEntry::kKindSpecial: return "special";
        case DirEntry::kKindDirectory: return "dir";
        default: return "?";
    }
}

void print_listing(const std::vector<DirEntry>& entries) {
    out("  {:<32} {:<4} {:<7} {:>10} {:>7} {:>8} {:>7}\n", "Name", "Type", "Kind", "Bytes",
        "Blocks", "Block", "Copies");
    for (const DirEntry& e : entries) {
        out("  {:<32} {:<4} {:<7} {:>10} {:>7} {:>8} {:>7}\n", e.name, e.type, kind_name(e),
            e.byte_count, e.block_count, e.avatars.front(), e.avatars.size());
    }
    out("  {} entries\n", entries.size());
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fputs("usage: opera-ls <image.iso|image.bin> [directory path]\n", stderr);
        return 2;
    }

    auto disc = core::cdrom::DiscImage::open(argv[1]);
    if (!disc) return 1;

    auto fs = core::cdrom::OperaFs::mount(*disc);
    if (!fs) return 1;

    const auto& label = fs->label();
    out("Image:   {} ({}, {} sectors)\n", argv[1],
        core::cdrom::DiscImage::format_name(disc->format()), disc->sector_count());
    out("Volume:  \"{}\"  id 0x{:08X}  flags 0x{:02X}\n", label.identifier, label.unique_id,
        label.flags);
    if (!label.commentary.empty()) out("Comment: \"{}\"\n", label.commentary);
    out("Blocks:  {} x {} bytes\n", label.block_count, label.block_size);
    out("Root:    {} block(s) at block {} ({} copies)\n\n", label.root_block_count,
        label.root_avatars.front(), label.root_avatars.size());

    std::optional<std::vector<DirEntry>> entries;
    if (argc > 2) {
        const auto dir = fs->find(argv[2]);
        if (!dir || !dir->is_directory()) {
            std::fprintf(stderr, "'%s': no such directory\n", argv[2]);
            return 1;
        }
        out("Directory {}:\n", argv[2]);
        entries = fs->read_directory(*dir);
    } else {
        out("Root directory:\n");
        entries = fs->read_root();
    }
    if (!entries) return 1;
    print_listing(*entries);

    if (const auto launchme = fs->find_launchme()) {
        out("\nBoot file: {} ({} bytes at block {})\n", launchme->name, launchme->byte_count,
            launchme->avatars.front());
    } else {
        out("\nBoot file: LaunchMe not found in root\n");
    }
    return 0;
}
