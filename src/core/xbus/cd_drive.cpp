#include "core/xbus/cd_drive.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "common/log.hpp"

namespace core::xbus {

namespace {

enum Command : u8 {
    kSeek = 0x01, kSpinUp = 0x02, kSpinDown = 0x03, kDiagnostics = 0x04,
    kEject = 0x06, kInject = 0x07, kAbort = 0x08, kModeSet = 0x09,
    kReset = 0x0A, kFlush = 0x0B, kPlayMsf = 0x0E, kPlayTrack = 0x0F,
    kReadData = 0x10, kDataPathCheck = 0x80, kGetLastStatus = 0x82,
    kReadId = 0x83, kModeSense = 0x84, kReadCapacity = 0x85, kReadHeader = 0x86,
    kReadSubQ = 0x87, kReadUpc = 0x88, kReadIsrc = 0x89, kReadDiscCode = 0x8A,
    kReadDiscInfo = 0x8B, kReadToc = 0x8C, kReadSessionInfo = 0x8D,
    kReadDeviceDriver = 0x8E,
};

constexpr u8 kAbortFlushStatusLength = 31;
constexpr u32 kMsfBiasFrames = 150;          // LBA 0 is 00:02:00
constexpr u8 kTocDataTrack = 0x14;           // CD_CTL_DATA_TRACK | CD_CTL_Q_POSITION
constexpr u8 kBlockFlagSubcode = 0x40;
constexpr u8 kBlockFlagError = 0x80;
constexpr u32 kSubcodeBytes = 96;
constexpr u32 kMaxBlockBytes = 2449;

struct Msf {
    u8 minutes, seconds, frames;
};

constexpr Msf lba_to_msf(u32 lba) {
    const u32 f = lba + kMsfBiasFrames;
    return {static_cast<u8>(f / (60 * 75)), static_cast<u8>((f / 75) % 60), static_cast<u8>(f % 75)};
}

constexpr u32 msf_to_lba(u8 m, u8 s, u8 f) {
    const u32 frames = (u32{m} * 60 + s) * 75 + f;
    return frames < kMsfBiasFrames ? 0 : frames - kMsfBiasFrames;
}

Log::Limiter raw_read_log("CD drive: non-2048-byte block read", 2);

// Debugging aid: EMU_TRACE_CD=1 logs every command and its response.
bool trace_enabled() {
    static const bool enabled = std::getenv("EMU_TRACE_CD") != nullptr;
    return enabled;
}

}  // namespace

void CdDrive::reset() {
    command_ = {};
    command_length_ = 0;
    status_ = {};
    status_length_ = status_read_ = 0;
    poll_ = poll::kIntEnMask;
    last_error_ = kNoError;
    density_ = 0;
    block_length_ = 2048;
    block_flags_ = 0;
    recovery_type_ = 0;
    retry_count_ = 8;
    stop_time_ = 0;
    speed_ = 0x80;
    pitch_msb_ = pitch_lsb_ = 0;
    chunk_size_ = 1;
    clear_transfer();
    current_lba_ = 0;
    // Door closed; a disc is already spinning if one is loaded (Opera).
    drive_status_ = kStatusReady | kStatusDoorClosed | kStatusDoubleSpeed;
    if (disc_) drive_status_ |= kStatusDiscIn | kStatusSpinUp;
}

void CdDrive::insert_disc(std::unique_ptr<cdrom::DiscImage> disc) {
    disc_ = std::move(disc);
    reset();
}

void CdDrive::open_tray() {
    clear_transfer();
    disc_.reset();
    drive_status_ &= static_cast<u8>(~(kStatusDoorClosed | kStatusDiscIn | kStatusSpinUp));
    drive_status_ |= kStatusReady;
    poll_ |= poll::kMediaChanged;
}

void CdDrive::close_tray(std::unique_ptr<cdrom::DiscImage> disc) {
    disc_ = std::move(disc);
    current_lba_ = 0;
    drive_status_ |= kStatusDoorClosed | kStatusReady;
    if (disc_) drive_status_ |= kStatusDiscIn | kStatusSpinUp;  // spins up on load, as Opera's inject
    last_error_ = kMediaChangedError;
    poll_ |= poll::kMediaChanged;
}

bool CdDrive::interrupt_pending() const {
    return ((poll_ & poll::kStatusValid) && (poll_ & poll::kStatusIntEn)) ||
           ((poll_ & poll::kDataValid) && (poll_ & poll::kDataIntEn)) ||
           ((poll_ & poll::kMediaAccess) && (poll_ & poll::kMediaIntEn));
}

u32 CdDrive::sector_cycles() const {
    const u32 speed = (speed_ == 0x80) ? 2 : 1;
    return kCpuClockHz / (kFramesPerSecond * speed);
}

u32 CdDrive::block_size() const {
    u32 size = block_length_ == 0 ? 2048u : block_length_;
    if (block_flags_ & kBlockFlagSubcode) size += kSubcodeBytes;
    if (block_flags_ & kBlockFlagError) size += 1;
    return std::min(size, kMaxBlockBytes);
}

void CdDrive::clear_transfer() {
    blocks_requested_ = 0;
    read_status_pending_ = false;
    data_.clear();
    data_read_ = 0;
    poll_ &= static_cast<u8>(~poll::kDataValid);
}

void CdDrive::write_command(u8 value) {
    if (command_length_ == 0) {
        // A new command discards any unread status.
        status_length_ = status_read_ = 0;
        poll_ &= static_cast<u8>(~poll::kStatusValid);
    }
    if (command_length_ < command_.size()) command_[command_length_++] = value;
    if (command_length_ >= command_.size() || command_[0] == kAbort) {
        execute();
        command_length_ = 0;
    }
}

u8 CdDrive::read_status() {
    if (status_read_ >= status_length_) return 0;
    const u8 value = status_[status_read_++];
    if (status_read_ >= status_length_) {
        status_length_ = status_read_ = 0;
        poll_ &= static_cast<u8>(~poll::kStatusValid);
    }
    return value;
}

u8 CdDrive::read_data() {
    if (data_read_ >= data_.size()) return 0;
    const u8 value = data_[data_read_++];
    if (data_read_ >= data_.size()) {
        data_.clear();
        data_read_ = 0;
        poll_ &= static_cast<u8>(~poll::kDataValid);
        // The next sector follows once the drive has read it (advance()).
    }
    return value;
}

void CdDrive::write_poll(u32 value) {
    if (value & poll::kReset) {
        status_length_ = status_read_ = 0;
        command_length_ = 0;
        poll_ &= static_cast<u8>(~poll::kStatusValid);
        clear_transfer();
    }
    if (value & poll::kResetDone) poll_ &= static_cast<u8>(~poll::kResetDone);
    poll_ = static_cast<u8>((poll_ & 0xF0) | (value & poll::kIntEnMask));
}

void CdDrive::advance(u32 cpu_cycles) {
    if (blocks_requested_ == 0) return;
    cycles_until_sector_ -= cpu_cycles;
    if (cycles_until_sector_ <= 0 && data_.empty()) {
        load_next_sector();
        cycles_until_sector_ += sector_cycles();
        if (cycles_until_sector_ < 0) cycles_until_sector_ = 0;
    }
}

void CdDrive::load_next_sector() {
    if (trace_enabled()) Log::info("CD   sector {} ({} left)", current_lba_, blocks_requested_ - 1);
    cdrom::Sector sector{};
    if (!disc_ || !disc_->read_sector(current_lba_, sector)) sector.fill(0);
    ++current_lba_;
    --blocks_requested_;

    const u32 size = block_size();
    data_.assign(size, 0);
    if (size != cdrom::kSectorSize)
        raw_read_log.warn("CD drive: {}-byte blocks requested; only 2048 bytes of user data "
                          "are available from the image",
                          size);
    std::copy_n(sector.begin(), std::min<std::size_t>(size, sector.size()), data_.begin());
    data_read_ = 0;
    poll_ |= poll::kDataValid;

    // The READ DATA status is released together with the first sector.
    if (read_status_pending_) {
        read_status_pending_ = false;
        poll_ |= poll::kStatusValid;
    }
}

void CdDrive::respond(u8 opcode, std::initializer_list<u8> payload, u8 error) {
    status_length_ = 0;
    status_read_ = 0;
    status_[status_length_++] = opcode;
    for (u8 b : payload) status_[status_length_++] = b;
    status_[status_length_++] = drive_status_;
    if (trace_enabled()) {
        std::string bytes;
        for (u8 i = 0; i < status_length_; ++i) bytes += std::format(" {:02X}", status_[i]);
        Log::info("CD   ->{} (error {:02X})", bytes, error);
    }
    last_error_ = error;
    poll_ |= poll::kStatusValid;
}

void CdDrive::respond_padded(u8 opcode, u8 total_length) {
    status_.fill(0);
    status_[0] = opcode;
    status_[total_length - 1] = drive_status_;
    status_length_ = total_length;
    status_read_ = 0;
    last_error_ = kNoError;
    poll_ |= poll::kStatusValid;
}

void CdDrive::respond_error(u8 opcode, u8 error) {
    drive_status_ = static_cast<u8>((drive_status_ | kStatusError) & ~kStatusReady);
    respond(opcode, {}, error);
}

void CdDrive::media_error(u8 opcode) {
    respond_error(opcode, disc_present() ? kNotReady : kDiscOut);
}

void CdDrive::cmd_mode_set() {
    switch (command_[1]) {
        case 0: {  // block: density, length, flags
            const u8 density = command_[2];
            const u16 length = static_cast<u16>((command_[3] << 8) | command_[4]);
            const u8 flags = command_[5];
            const bool density_ok = density == 0x00 || density == 0x01 || density == 0x81 || density == 0x82;
            if (!density_ok || (flags & ~(kBlockFlagSubcode | kBlockFlagError)) || length > kMaxBlockBytes) {
                respond_error(kModeSet, kModeError);
                return;
            }
            density_ = density;
            block_length_ = length;
            block_flags_ = flags;
            break;
        }
        case 1: recovery_type_ = command_[2]; retry_count_ = command_[3]; break;
        case 2: stop_time_ = command_[2]; break;
        case 3:
            speed_ = command_[2];
            pitch_msb_ = command_[3];
            pitch_lsb_ = command_[4];
            if (speed_ == 0x80) drive_status_ |= kStatusDoubleSpeed;
            else drive_status_ &= static_cast<u8>(~kStatusDoubleSpeed);
            break;
        case 4: chunk_size_ = command_[2]; break;
        default: respond_error(kModeSet, kModeError); return;
    }
    drive_status_ |= kStatusReady;
    respond(kModeSet, {});
}

void CdDrive::cmd_mode_sense() {
    if (!disc_present()) {
        media_error(kModeSense);
        return;
    }
    drive_status_ |= kStatusReady;
    switch (command_[1]) {
        case 0: respond(kModeSense, {density_, static_cast<u8>(block_length_ >> 8), static_cast<u8>(block_length_)}); break;
        case 1: respond(kModeSense, {recovery_type_, retry_count_, 0}); break;
        case 2: respond(kModeSense, {stop_time_, 0, 0}); break;
        case 3: respond(kModeSense, {speed_, pitch_msb_, pitch_lsb_}); break;
        case 4: respond(kModeSense, {chunk_size_, 0, 0}); break;
        default: respond(kModeSense, {0, 0, 0}); break;
    }
}

void CdDrive::cmd_read_data() {
    if (!spinning()) {
        media_error(kReadData);
        return;
    }
    const u32 lba = msf_to_lba(command_[1], command_[2], command_[3]);
    const u32 blocks = (u32{command_[4]} << 16) | (u32{command_[5]} << 8) | command_[6];
    if (command_[2] >= 60 || command_[3] >= 75) {
        clear_transfer();
        respond_error(kReadData, kAddressError);
        return;
    }
    const u32 disc_blocks = disc_->sector_count();
    if (blocks != 0 && (lba >= disc_blocks || blocks > disc_blocks - lba)) {
        clear_transfer();
        respond_error(kReadData, kEndAddress);
        return;
    }

    clear_transfer();
    drive_status_ |= kStatusReady;
    status_ = {};
    status_[0] = kReadData;
    status_[1] = drive_status_;
    status_length_ = 2;
    status_read_ = 0;
    last_error_ = kNoError;
    current_lba_ = lba;
    blocks_requested_ = blocks;
    if (blocks == 0) {
        poll_ |= poll::kStatusValid;
        return;
    }
    // Status is published with the first sector (see load_next_sector).
    read_status_pending_ = true;
    cycles_until_sector_ = sector_cycles();
}

void CdDrive::cmd_read_toc() {
    if (!spinning()) {
        media_error(kReadToc);
        return;
    }
    const Msf total = lba_to_msf(disc_->sector_count());
    const u8 track = command_[2];
    drive_status_ |= kStatusReady;
    if (track == 0xAA)  // lead-out
        respond(kReadToc, {0, kTocDataTrack, 0xAA, 0, total.minutes, total.seconds, total.frames, 0});
    else if (track == 1)
        respond(kReadToc, {0, kTocDataTrack, 1, 0, 0, 2, 0, 0});  // track 1 at 00:02:00
    else
        respond_error(kReadToc, kTrackError);
}

void CdDrive::cmd_read_subq() {
    if (!spinning()) {
        media_error(kReadSubQ);
        return;
    }
    const Msf absolute = lba_to_msf(current_lba_);
    const u32 rel = current_lba_;
    const u8 ctl_adr = static_cast<u8>(((kTocDataTrack << 4) & 0xF0) | ((kTocDataTrack >> 4) & 0x0F));
    drive_status_ |= kStatusReady;
    respond(kReadSubQ, {0, ctl_adr, 0x01, 0x01, absolute.minutes, absolute.seconds, absolute.frames,
                        static_cast<u8>(rel / (60 * 75)), static_cast<u8>((rel / 75) % 60),
                        static_cast<u8>(rel % 75)});
}

void CdDrive::execute() {
    const u8 opcode = command_[0];
    if (trace_enabled())
        Log::info("CD cmd {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}", command_[0], command_[1],
                  command_[2], command_[3], command_[4], command_[5], command_[6]);
    poll_ &= static_cast<u8>(~(poll::kStatusValid | poll::kDataValid));
    drive_status_ &= static_cast<u8>(~(kStatusError | kStatusReady));
    auto ready = [&] { drive_status_ |= kStatusReady; };

    switch (opcode) {
        case kSeek:
        case kDiagnostics:
        case kReadHeader:
        case kReadUpc:
        case kReadIsrc:
        case kReadDeviceDriver:
            respond_error(opcode, kIllegalRequest);
            break;

        case kSpinUp:
        case kSpinDown:
            if (!disc_present()) {
                respond_error(opcode, kDiscOut);
                break;
            }
            if (opcode == kSpinUp) drive_status_ |= kStatusSpinUp;
            else drive_status_ &= static_cast<u8>(~kStatusSpinUp);
            ready();
            respond(opcode, {});
            break;

        case kEject:
            clear_transfer();
            ready();
            drive_status_ &= static_cast<u8>(~(kStatusDoorClosed | kStatusDiscIn | kStatusSpinUp));
            respond(opcode, {});
            break;
        case kInject:  // closes the tray, loading the disc if there is one
            clear_transfer();
            ready();
            drive_status_ |= kStatusDoorClosed;
            if (disc_) drive_status_ |= kStatusDiscIn | kStatusSpinUp;
            respond(opcode, {});
            break;

        case kAbort:
        case kFlush:
            clear_transfer();
            ready();
            respond_padded(opcode, kAbortFlushStatusLength);
            break;

        case kModeSet: clear_transfer(); cmd_mode_set(); break;
        case kReset:
            clear_transfer();
            ready();
            respond(opcode, {});
            break;

        case kDataPathCheck:
            ready();
            respond(opcode, {0xAA, 0x55});
            break;

        case kGetLastStatus: {
            const u8 e = last_error_;
            ready();
            respond(opcode, {e, e, e, e, e, e, e, e}, e);
            last_error_ = kNoError;
            break;
        }

        case kReadId:
            // Manufacturer 0x0010 (MEI), device 0x0001 (CD-ROM), no revision,
            // flags or driver tag table: Opera's native MEI persona.
            ready();
            respond(opcode, {0x00, 0x10, 0x00, 0x01, 0, 0, 0, 0, 0, 0});
            break;

        case kModeSense: cmd_mode_sense(); break;
        case kReadData: cmd_read_data(); break;
        case kReadToc: cmd_read_toc(); break;
        case kReadSubQ: cmd_read_subq(); break;

        case kReadCapacity:
            if (!spinning()) { media_error(opcode); break; }
            {
                const Msf total = lba_to_msf(disc_->sector_count());
                ready();
                respond(opcode, {0, total.minutes, total.seconds, total.frames, 0, 0});
            }
            break;
        case kReadDiscInfo:
            if (!spinning()) { media_error(opcode); break; }
            {
                const Msf total = lba_to_msf(disc_->sector_count());
                ready();
                // Disc type 0 (CD-DA / CD-ROM), tracks 1..1, lead-out position.
                respond(opcode, {0x00, 1, 1, total.minutes, total.seconds, total.frames});
            }
            break;
        case kReadSessionInfo:
            if (!disc_present()) { media_error(opcode); break; }
            ready();
            respond(opcode, {0, 0, 0, 0, 0, 0});  // single session
            break;
        case kReadDiscCode:
            if (!spinning()) { media_error(opcode); break; }
            ready();
            respond(opcode, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
            break;

        case kPlayMsf:
        case kPlayTrack:
            // CD audio playback is not emulated; 3DO software streams audio
            // through READ DATA instead (Opera notes the same).
            if (!spinning()) { media_error(opcode); break; }
            ready();
            respond(opcode, {});
            break;

        default:
            Log::warn("CD drive: unhandled command {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                      command_[0], command_[1], command_[2], command_[3], command_[4],
                      command_[5], command_[6]);
            respond_error(opcode, kCmdError);
            break;
    }
}

}  // namespace core::xbus
