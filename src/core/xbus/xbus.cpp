#include "core/xbus/xbus.hpp"

namespace core::xbus {

namespace {

constexpr u8 kCmdReadId = 0x83;
constexpr u8 kStatusOk = 0xE1;

}  // namespace

Xbus::Xbus(std::function<void()> raise_interrupt) : raise_interrupt_(std::move(raise_interrupt)) {
    reset();
}

void Xbus::reset() {
    cd_drive_.reset();  // keeps any inserted disc
    select_low_ = select_high_ = 0;
    poll_ = poll::kIntEnMask;
    bus_poll_flags_ = 0;
    command_ = {};
    command_length_ = 0;
    status_ = {};
    status_length_ = status_read_ = 0;
}

bool Xbus::access_aborts(u32 offset) const {
    return offset >= kPoll && offset < kEnd && selected_absent();
}

bool Xbus::bus_interrupt_pending() const {
    const u8 p = bus_poll();
    return ((p & poll::kStatusValid) && (p & poll::kStatusIntEn)) ||
           ((p & poll::kDataValid) && (p & poll::kDataIntEn)) ||
           ((p & poll::kMediaAccess) && (p & poll::kMediaIntEn));
}

void Xbus::check_interrupt() {
    const bool pending = cd_drive_.interrupt_pending() ||
                         (select_low_ == kBusSelect && bus_interrupt_pending());
    if (pending && raise_interrupt_) raise_interrupt_();
}

void Xbus::tick(u32 cpu_cycles) {
    cd_drive_.advance(cpu_cycles);
    check_interrupt();
}

u8 Xbus::read_data() {
    return device_present(select_low_) ? cd_drive_.read_data() : 0;
}

u32 Xbus::read(u32 offset) {
    if (offset < kPoll) return 0;  // reserved

    if (offset < kFifoCommandStatus) {
        u32 value = 0;
        if (select_low_ == kBusSelect) value = bus_poll();
        else if (device_present(select_low_)) value = cd_drive_.poll();
        if (select_high_ & 0x80) value &= 0x0F;
        return value;
    }

    if (offset < kFifoData) {
        if (device_present(select_low_)) return cd_drive_.read_status();
        if (select_low_ == kBusSelect && status_read_ < status_length_) {
            const u8 value = status_[status_read_++];
            if (status_read_ >= status_length_) {
                status_length_ = status_read_ = 0;
                bus_poll_flags_ &= static_cast<u8>(~poll::kStatusValid);
            }
            return value;
        }
        return 0;
    }

    return read_data();  // data FIFO
}

void Xbus::write(u32 offset, u32 value) {
    if (offset < kPoll) {
        select_low_ = static_cast<u8>(value & 0x0F);
        select_high_ = static_cast<u8>(value & 0xF0);
        return;
    }

    if (offset < kFifoCommandStatus) {
        if (select_low_ == kBusSelect) {
            if (value & poll::kReset) {
                status_length_ = status_read_ = 0;
                command_length_ = 0;
                bus_poll_flags_ &= static_cast<u8>(~(poll::kStatusValid | poll::kDataValid |
                                                     poll::kMediaAccess));
            }
            if (value & poll::kResetDone) bus_poll_flags_ &= static_cast<u8>(~poll::kResetDone);
            poll_ = static_cast<u8>((poll_ & 0xF0) | (value & poll::kIntEnMask));
        }
        if (device_present(select_low_)) cd_drive_.write_poll(value);
        check_interrupt();
        return;
    }

    if (offset < kFifoData) {
        if (device_present(select_low_)) {
            cd_drive_.write_command(static_cast<u8>(value));
        } else if (select_low_ == kBusSelect) {
            if (command_length_ < command_.size()) command_[command_length_++] = static_cast<u8>(value);
            if (command_length_ >= command_.size()) {
                if (command_[0] == kCmdReadId) {
                    // Opera's bus-level READ ID: an MEI CD-ROM, status OK.
                    status_ = {};
                    status_[0] = kCmdReadId;
                    status_[2] = 0x10;
                    status_[4] = 0x01;
                    status_[11] = kStatusOk;
                    status_length_ = static_cast<u8>(status_.size());
                    status_read_ = 0;
                    bus_poll_flags_ |= poll::kStatusValid;
                }
                command_length_ = 0;
            }
        }
        check_interrupt();
        return;
    }

    // Data FIFO writes: the drive accepts no data yet.
}

}  // namespace core::xbus
