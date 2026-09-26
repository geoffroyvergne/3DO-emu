#pragma once

#include <array>

#include "common/types.hpp"

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
inline constexpr u8 kResetDone = 0x80;    // POLRE
inline constexpr u8 kIntEnMask = kStatusIntEn | kDataIntEn | kMediaIntEn;
}  // namespace poll

// The console's MEI CD-ROM drive, as XBUS device 0. Commands are 7 bytes
// (ABORT is a single byte); each response is a status packet of
// [opcode, payload..., drive status] read back one byte at a time.
// Protocol, codes and responses follow Opera's opera_cdrom.c.
//
// Only the no-disc state is modelled for now: the drive is present with its
// door closed, and every media command fails with "disc out".
class CdDrive {
public:
    // Drive status byte (last byte of each response).
    static constexpr u8 kStatusDoorClosed = 0x80;
    static constexpr u8 kStatusDiscIn = 0x40;
    static constexpr u8 kStatusSpinUp = 0x20;
    static constexpr u8 kStatusError = 0x10;
    static constexpr u8 kStatusReady = 0x01;

    void reset();

    void write_command(u8 value);
    u8 read_status();
    void write_poll(u32 value);
    [[nodiscard]] u8 poll() const { return poll_; }
    [[nodiscard]] bool interrupt_pending() const;

    [[nodiscard]] u8 drive_status() const { return drive_status_; }

private:
    enum MeiError : u8 {
        kNoError = 0x00,
        kNotReady = 0x03,
        kCmdError = 0x14,
        kDiscOut = 0x15,
        kIllegalRequest = 0x17,
    };

    void execute();
    void respond(u8 opcode, std::initializer_list<u8> payload, u8 error = kNoError);
    void respond_padded(u8 opcode, u8 total_length);
    void respond_error(u8 opcode, u8 error);
    [[nodiscard]] bool disc_present() const {
        return (drive_status_ & (kStatusDoorClosed | kStatusDiscIn)) ==
               (kStatusDoorClosed | kStatusDiscIn);
    }

    std::array<u8, 7> command_{};
    u8 command_length_ = 0;
    std::array<u8, 64> status_{};
    u8 status_length_ = 0;
    u8 status_read_ = 0;
    u8 poll_ = poll::kIntEnMask;
    u8 drive_status_ = 0;
    u8 last_error_ = kNoError;
};

}  // namespace core::xbus
