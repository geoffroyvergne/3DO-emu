#pragma once

#include <array>
#include <functional>

#include "common/types.hpp"
#include "core/xbus/cd_drive.hpp"

namespace core::xbus {

// The expansion bus ("XBUS"), reached through CLIO +0x500..+0x5FF:
//   +0x500 write: select device (low nibble), read: reserved (0)
//   +0x540 poll/control of the selected device
//   +0x580 write: command FIFO, read: status FIFO
//   +0x5C0 data FIFO
// Device 0 is the built-in CD drive. Selecting 0xF addresses the bus itself
// (Opera answers READ ID there). Accessing +0x540..+0x5FF while an absent
// device is selected aborts; the OS uses this to probe the bus.
// Behaviour follows Opera's opera_xbus.c.
class Xbus {
public:
    static constexpr u32 kSelect = 0x500;
    static constexpr u32 kPoll = 0x540;
    static constexpr u32 kFifoCommandStatus = 0x580;
    static constexpr u32 kFifoData = 0x5C0;
    static constexpr u32 kEnd = 0x600;

    // `raise_interrupt` sets CLIO's expansion-bus interrupt (Int0 EXINT).
    explicit Xbus(std::function<void()> raise_interrupt);

    void reset();

    [[nodiscard]] bool access_aborts(u32 offset) const;
    u32 read(u32 offset);
    void write(u32 offset, u32 value);

    // Called periodically with the emulated time elapsed: advances the drive
    // and re-raises the interrupt while a device needs service.
    void tick(u32 cpu_cycles);

    // One byte from the selected device's data FIFO (also used by CLIO's
    // expansion-bus DMA).
    u8 read_data();

    [[nodiscard]] CdDrive& cd_drive() { return cd_drive_; }

private:
    static constexpr u8 kBusSelect = 0x0F;

    [[nodiscard]] bool device_present(u8 index) const { return index == 0; }
    [[nodiscard]] bool selected_absent() const {
        return select_low_ != kBusSelect && !device_present(select_low_);
    }
    [[nodiscard]] u8 bus_poll() const { return static_cast<u8>((poll_ & 0x0F) | (bus_poll_flags_ & 0xF0)); }
    [[nodiscard]] bool bus_interrupt_pending() const;
    void check_interrupt();

    std::function<void()> raise_interrupt_;
    CdDrive cd_drive_;
    u8 select_low_ = 0;
    u8 select_high_ = 0;
    // Bus-level (select 0xF) state.
    u8 poll_ = poll::kIntEnMask;
    u8 bus_poll_flags_ = 0;
    std::array<u8, 7> command_{};
    u8 command_length_ = 0;
    std::array<u8, 12> status_{};
    u8 status_length_ = 0;
    u8 status_read_ = 0;
};

}  // namespace core::xbus
