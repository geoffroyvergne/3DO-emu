#pragma once

#include <array>
#include <span>

#include "common/types.hpp"
#include "core/bus/memory.hpp"

namespace core::slowbus {

// The "slow bus" window (0x03100000, Portfolio's SLOWBUS): battery-backed
// NVRAM and the diagnostic port. Layout from Opera's opera_arm.c:
//   offset & 0x80000  diagnostic port (reads 0: no diagnostics requested)
//   offset & 0x40000  NVRAM, 32 KB, one byte per word: NVRAM[(offset >> 2) & 0x7FFF]
class SlowBus final : public MmioDevice {
public:
    static constexpr std::size_t kNvramSize = 32 * 1024;

    void reset() {}  // NVRAM is battery backed: it survives resets

    [[nodiscard]] std::span<u8> nvram() { return nvram_; }

    u32 mmio_read32(u32 offset) override;
    void mmio_write32(u32 offset, u32 value) override;

private:
    static constexpr u32 kDiagPort = 0x80000;
    static constexpr u32 kNvramSelect = 0x40000;

    std::array<u8, kNvramSize> nvram_{};
};

}  // namespace core::slowbus
