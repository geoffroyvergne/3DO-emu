#pragma once

#include "common/types.hpp"

namespace core {

// CPU-facing memory interface. Values cross this boundary in host order:
// implementations own the big-endian <-> host conversion of the 3DO memory.
// The concrete 3DO memory map (RAM/VRAM/ROM/MMIO) implements this in step 2.
class Bus {
public:
    virtual ~Bus() = default;

    virtual u32 read32(u32 address) = 0;
    virtual void write32(u32 address, u32 value) = 0;
    virtual u8 read8(u32 address) = 0;
    virtual void write8(u32 address, u8 value) = 0;

    // Instruction fetch. Same data as read32, but lets the bus tell a CPU
    // executing from the wrong place apart from a genuine register read.
    virtual u32 fetch32(u32 address) { return read32(address); }

    // True (once) if the last data access was refused by the hardware, which
    // the CPU turns into a data abort. Reads return 0 and writes are dropped.
    virtual bool consume_abort() { return false; }
};

}  // namespace core
