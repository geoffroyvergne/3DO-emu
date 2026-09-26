#pragma once

#include <span>

#include "common/types.hpp"
#include "core/bus/memory.hpp"

namespace core::sport {

// SPORT: the VRAM serial port (0x03200000). Moves or fills whole 2 KB VRAM
// pages in one access, used for screen clears and copies. The BIOS POST
// (test #7) checks it. Behaviour follows Opera's opera_sport.c:
//   read  +idx (idx < 0x2000): select source page (idx & 0x7FF) << 9
//   write +idx, value = bit mask:
//     idx & 0xE000 == 0x0000  copy source page -> page(idx)
//                    0x2000  set fill colour (value)
//                    0x4000  fill page(idx) with the colour ("flash write")
// A mask other than 0xFFFFFFFF merges per bit, as Opera does.
class Sport final : public MmioDevice {
public:
    static constexpr u32 kPageBytes = 2048;

    explicit Sport(std::span<u8> vram) : vram_(vram) {}

    void reset();

    u32 mmio_read32(u32 offset) override;
    void mmio_write32(u32 offset, u32 value) override;

private:
    static constexpr u32 page_offset(u32 index) { return (index & 0x7FF) << 9; }
    [[nodiscard]] u32 word(u32 byte_offset) const;
    void set_word(u32 byte_offset, u32 value);

    void copy_page(u32 index, u32 mask);
    void fill_page(u32 index, u32 mask);

    std::span<u8> vram_;
    u32 color_ = 0;
    u32 source_ = 0;       // byte offset into VRAM
    u32 destination_ = 0;  // byte offset into VRAM
};

}  // namespace core::sport
