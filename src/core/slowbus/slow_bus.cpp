#include "core/slowbus/slow_bus.hpp"

#include "common/log.hpp"

namespace core::slowbus {

namespace {

Log::Limiter unmapped_log("Slow bus access outside NVRAM/diag", 8);

}  // namespace

u32 SlowBus::mmio_read32(u32 offset) {
    if (offset & kDiagPort) return 0;
    if (offset & kNvramSelect) return nvram_[(offset >> 2) & (kNvramSize - 1)];
    unmapped_log.warn("Slow bus read at +0x{:05X}", offset);
    return 0;
}

void SlowBus::mmio_write32(u32 offset, u32 value) {
    if (offset & kDiagPort) return;  // diagnostic output: ignored
    if (offset & kNvramSelect) {
        nvram_[(offset >> 2) & (kNvramSize - 1)] = static_cast<u8>(value);
        return;
    }
    unmapped_log.warn("Slow bus write at +0x{:05X} = 0x{:08X}", offset, value);
}

}  // namespace core::slowbus
