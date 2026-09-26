#include "core/xbus/cd_drive.hpp"

#include <algorithm>

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

}  // namespace

void CdDrive::reset() {
    command_ = {};
    command_length_ = 0;
    status_ = {};
    status_length_ = 0;
    status_read_ = 0;
    poll_ = poll::kIntEnMask;
    // Present, door closed, no disc.
    drive_status_ = kStatusReady | kStatusDoorClosed;
    last_error_ = kNoError;
}

bool CdDrive::interrupt_pending() const {
    return ((poll_ & poll::kStatusValid) && (poll_ & poll::kStatusIntEn)) ||
           ((poll_ & poll::kDataValid) && (poll_ & poll::kDataIntEn)) ||
           ((poll_ & poll::kMediaAccess) && (poll_ & poll::kMediaIntEn));
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

void CdDrive::write_poll(u32 value) {
    if (value & poll::kReset) {
        status_length_ = status_read_ = 0;
        command_length_ = 0;
        poll_ &= static_cast<u8>(~poll::kStatusValid);
    }
    if (value & poll::kResetDone) poll_ &= static_cast<u8>(~poll::kResetDone);
    poll_ = static_cast<u8>((poll_ & 0xF0) | (value & poll::kIntEnMask));
}

void CdDrive::respond(u8 opcode, std::initializer_list<u8> payload, u8 error) {
    status_length_ = 0;
    status_read_ = 0;
    status_[status_length_++] = opcode;
    for (u8 b : payload) status_[status_length_++] = b;
    status_[status_length_++] = drive_status_;
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

void CdDrive::execute() {
    const u8 opcode = command_[0];
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
            ready();
            drive_status_ &= static_cast<u8>(~(kStatusDoorClosed | kStatusDiscIn | kStatusSpinUp));
            respond(opcode, {});
            break;
        case kInject:  // closes the tray; still no disc to load
            ready();
            drive_status_ |= kStatusDoorClosed;
            respond(opcode, {});
            break;

        case kAbort:
        case kFlush:
            ready();
            respond_padded(opcode, kAbortFlushStatusLength);
            break;

        case kModeSet:
        case kReset:
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

        case kPlayMsf:
        case kPlayTrack:
        case kReadData:
        case kModeSense:
        case kReadCapacity:
        case kReadSubQ:
        case kReadDiscCode:
        case kReadDiscInfo:
        case kReadToc:
        case kReadSessionInfo:
            // Every media command: no disc in the drive.
            respond_error(opcode, disc_present() ? kNotReady : kDiscOut);
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
