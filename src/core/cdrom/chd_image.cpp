// CHD (MAME "Compressed Hunks of Data") disc images, read through libchdr.
//
// A CD CHD stores frames of 2352 bytes of sector data + 96 bytes of
// subcode, grouped into compressed hunks. Track layout comes from CHT2
// metadata; each track is padded to a multiple of 4 frames. We expose the
// first data track as 2048-byte user sectors.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <libchdr/chd.h>

#include "common/log.hpp"
#include "core/cdrom/disc_image.hpp"

namespace core::cdrom {

namespace {

constexpr u32 kFrameBytes = 2352 + 96;
constexpr u32 kTrackPadding = 4;

struct TrackInfo {
    std::string type;  // MODE1_RAW, MODE1, MODE2_RAW, AUDIO, ...
    u32 frames = 0;
    u32 pregap = 0;
    bool pregap_in_data = false;  // PGTYPE starting with 'V': pregap frames are stored
};

std::vector<TrackInfo> read_tracks(chd_file* chd) {
    std::vector<TrackInfo> tracks;
    char text[512];
    for (u32 index = 0;; ++index) {
        u32 length = 0;
        const chd_error err = chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, index, text,
                                               sizeof(text) - 1, &length, nullptr, nullptr);
        if (err != CHDERR_NONE) break;
        text[std::min<u32>(length, sizeof(text) - 1)] = '\0';

        int number = 0, frames = 0, pregap = 0, postgap = 0;
        char type[64] = {}, subtype[64] = {}, pgtype[64] = {}, pgsub[64] = {};
        if (std::sscanf(text, CDROM_TRACK_METADATA2_FORMAT, &number, type, subtype, &frames, &pregap,
                        pgtype, pgsub, &postgap) < 4)
            break;
        tracks.push_back({type, static_cast<u32>(frames), static_cast<u32>(pregap), pgtype[0] == 'V'});
    }
    return tracks;
}

class ChdDiscImage final : public DiscImage {
public:
    ChdDiscImage(chd_file* chd, u32 hunk_bytes, u32 first_frame, u32 sector_count, u32 data_offset)
        : chd_(chd),
          frames_per_hunk_(hunk_bytes / kFrameBytes),
          first_frame_(first_frame),
          sector_count_(sector_count),
          data_offset_(data_offset),
          hunk_(hunk_bytes) {}

    ~ChdDiscImage() override { chd_close(chd_); }

    bool read_sector(u32 lba, std::span<u8, kSectorSize> out) override {
        if (lba >= sector_count_) {
            Log::warn("Disc read past end: sector {} of {}", lba, sector_count_);
            return false;
        }
        const u32 frame = first_frame_ + lba;
        const u32 hunk = frame / frames_per_hunk_;
        if (hunk != cached_hunk_) {
            const chd_error err = chd_read(chd_, hunk, hunk_.data());
            if (err != CHDERR_NONE) {
                Log::error("CHD: cannot decode hunk {}: {}", hunk, chd_error_string(err));
                cached_hunk_ = kNoHunk;
                return false;
            }
            cached_hunk_ = hunk;
        }
        const std::size_t at = std::size_t{frame % frames_per_hunk_} * kFrameBytes + data_offset_;
        std::memcpy(out.data(), &hunk_[at], kSectorSize);
        return true;
    }

    [[nodiscard]] u32 sector_count() const override { return sector_count_; }
    [[nodiscard]] Format format() const override { return Format::Chd; }

private:
    static constexpr u32 kNoHunk = 0xFFFFFFFF;

    chd_file* chd_;
    u32 frames_per_hunk_;
    u32 first_frame_;
    u32 sector_count_;
    u32 data_offset_;
    std::vector<u8> hunk_;
    u32 cached_hunk_ = kNoHunk;
};

}  // namespace

std::unique_ptr<DiscImage> open_chd_image(const std::filesystem::path& path) {
    chd_file* chd = nullptr;
    const chd_error err = chd_open(path.string().c_str(), CHD_OPEN_READ, nullptr, &chd);
    if (err != CHDERR_NONE) {
        Log::error("Cannot open CHD '{}': {}", path.string(), chd_error_string(err));
        return nullptr;
    }
    const chd_header* header = chd_get_header(chd);
    if (header->unitbytes != kFrameBytes || header->hunkbytes % kFrameBytes != 0) {
        Log::error("CHD '{}' is not a CD image (unit size {})", path.string(), header->unitbytes);
        chd_close(chd);
        return nullptr;
    }

    const std::vector<TrackInfo> tracks = read_tracks(chd);
    if (tracks.empty()) {
        Log::error("CHD '{}' has no CD track metadata", path.string());
        chd_close(chd);
        return nullptr;
    }

    // Locate the first data track; frames of earlier tracks are skipped.
    u32 frame = 0;
    for (const TrackInfo& track : tracks) {
        const u32 stored = track.frames;  // FRAMES includes a stored ('V') pregap
        const u32 start = frame + (track.pregap_in_data ? track.pregap : 0);
        const u32 sectors = stored - (track.pregap_in_data ? track.pregap : 0);
        u32 data_offset = 0;
        bool data = true;
        if (track.type == "MODE1_RAW" || track.type == "MODE1/2352") data_offset = 16;
        else if (track.type == "MODE2_RAW" || track.type == "MODE2/2352") data_offset = 24;
        else if (track.type == "MODE1" || track.type == "MODE1/2048") data_offset = 0;
        else data = false;

        if (data) {
            Log::info("CHD: data track {} ({}), {} sectors", &track - tracks.data() + 1, track.type,
                      sectors);
            return std::make_unique<ChdDiscImage>(chd, header->hunkbytes, start, sectors, data_offset);
        }
        frame += (stored + kTrackPadding - 1) / kTrackPadding * kTrackPadding;
    }

    Log::error("CHD '{}' has no data track", path.string());
    chd_close(chd);
    return nullptr;
}

}  // namespace core::cdrom
