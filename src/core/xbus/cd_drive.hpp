#pragma once

#include <array>
#include <memory>
#include <vector>

#include "common/types.hpp"
#include "core/cdrom/disc_image.hpp"

namespace core::xbus {

// XBUS poll register bits (shared by the bus and its devices).
namespace poll {
inline constexpr u8 kStatusIntEn = 0x01;  // POLSTMASK
inline constexpr u8 kDataIntEn = 0x02;    // POLDTMASK
inline constexpr u8 kMediaIntEn = 0x04;   // POLMAMASK
inline constexpr u8 kReset = 0x08;        // POLREMASK: flush FIFOs
inline constexpr u8 kStatusValid = 0x10;  // POLST
inline constexpr u8 kDataValid = 0x20;    // POLDT
inline constexpr u8 kMediaAccess = 0x40;  // POLMA
inline constexpr u8 kResetDone = 0x80;    // POLRE; Portfolio: XBUS_POLL_MEDIA_ACCESS (r/clear)
inline constexpr u8 kMediaChanged = kResetDone;  // latched when a disc is changed
inline constexpr u8 kIntEnMask = kStatusIntEn | kDataIntEn | kMediaIntEn;
}  // namespace poll

// The console's MEI CD-ROM drive, as XBUS device 0. Commands are 7 bytes
// (ABORT is a single byte); each response is a status packet of
// [opcode, payload..., drive status] read back one byte at a time. Sector
// data arrives in a separate data FIFO at the drive's speed.
// Protocol, codes and responses follow Opera's opera_cdrom.c; the disc is
// presented as one data track, as Opera does for plain images.
class CdDrive {
public:
    // Drive status byte (last byte of each response).
    static constexpr u8 kStatusDoorClosed = 0x80;
    static constexpr u8 kStatusDiscIn = 0x40;
    static constexpr u8 kStatusSpinUp = 0x20;
    static constexpr u8 kStatusError = 0x10;
    static constexpr u8 kStatusDoubleSpeed = 0x02;
    static constexpr u8 kStatusReady = 0x01;

    static constexpr u32 kCpuClockHz = 12'500'000;
    static constexpr u32 kFramesPerSecond = 75;

    void reset();

    // Inserts (or, with nullptr, removes) a disc before booting. The drive
    // keeps it across resets.
    void insert_disc(std::unique_ptr<cdrom::DiscImage> disc);
    [[nodiscard]] bool has_disc() const { return disc_ != nullptr; }

    // Changing discs while running: the tray opens (door open, no disc), then
    // closes on the new disc, latching the "media changed" poll bit that the
    // OS's CD driver watches for.
    void open_tray();
    void close_tray(std::unique_ptr<cdrom::DiscImage> disc);
    [[nodiscard]] bool tray_open() const { return !(drive_status_ & kStatusDoorClosed); }

    void write_command(u8 value);
    u8 read_status();
    u8 read_data();
    void write_poll(u32 value);
    [[nodiscard]] u8 poll() const { return poll_; }
    [[nodiscard]] bool interrupt_pending() const;

    // Advances the drive's clock: sectors become available at 75 x speed per second.
    void advance(u32 cpu_cycles);

    [[nodiscard]] u8 drive_status() const { return drive_status_; }
    [[nodiscard]] std::size_t data_available() const { return data_.size() - data_read_; }

private:
    enum MeiError : u8 {
        kNoError = 0x00,
        kNotReady = 0x03,
        kTrackError = 0x07,
        kEndAddress = 0x0F,
        kModeError = 0x10,
        kCmdError = 0x14,
        kDiscOut = 0x15,
        kIllegalRequest = 0x17,
        kAddressError = 0x0D,
        kMediaChangedError = 0x11,
    };

    void execute();
    void respond(u8 opcode, std::initializer_list<u8> payload, u8 error = kNoError);
    void respond_padded(u8 opcode, u8 total_length);
    void respond_error(u8 opcode, u8 error);
    [[nodiscard]] bool disc_present() const {
        return (drive_status_ & (kStatusDoorClosed | kStatusDiscIn)) ==
               (kStatusDoorClosed | kStatusDiscIn);
    }
    [[nodiscard]] bool spinning() const { return disc_present() && (drive_status_ & kStatusSpinUp); }
    void media_error(u8 opcode);
    void clear_transfer();
    void load_next_sector();
    [[nodiscard]] u32 sector_cycles() const;
    [[nodiscard]] u32 block_size() const;

    void cmd_mode_set();
    void cmd_mode_sense();
    void cmd_read_data();
    void cmd_read_toc();
    void cmd_read_subq();

    std::unique_ptr<cdrom::DiscImage> disc_;

    std::array<u8, 7> command_{};
    u8 command_length_ = 0;
    std::array<u8, 64> status_{};
    u8 status_length_ = 0;
    u8 status_read_ = 0;
    u8 poll_ = poll::kIntEnMask;
    u8 drive_status_ = 0;
    u8 last_error_ = kNoError;

    // Mode pages (MODE SET / MODE SENSE).
    u8 density_ = 0;
    u16 block_length_ = 2048;
    u8 block_flags_ = 0;
    u8 recovery_type_ = 0;
    u8 retry_count_ = 8;
    u8 stop_time_ = 0;
    u8 speed_ = 0x80;  // double speed
    u8 pitch_msb_ = 0, pitch_lsb_ = 0;
    u8 chunk_size_ = 1;

    // Data transfer.
    u32 current_lba_ = 0;
    u32 blocks_requested_ = 0;
    bool read_status_pending_ = false;
    std::vector<u8> data_;
    std::size_t data_read_ = 0;
    s64 cycles_until_sector_ = 0;
};

}  // namespace core::xbus
