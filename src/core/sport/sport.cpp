#include "core/sport/sport.hpp"

#include <cstring>

#include "common/endian.hpp"
#include "common/log.hpp"

namespace core::sport {

void Sport::reset() {
    color_ = 0;
    source_ = 0;
    destination_ = 0;
}

u32 Sport::word(u32 byte_offset) const {
    return load_be32(&vram_[byte_offset]);
}

void Sport::set_word(u32 byte_offset, u32 value) {
    store_be32(&vram_[byte_offset], value);
}

u32 Sport::mmio_read32(u32 offset) {
    if (offset < 0x2000) {
        source_ = page_offset(offset);
        return 0;
    }
    // Opera returns this marker for the rest of the window.
    static Log::Limiter log("SPORT read outside source-select range", 4);
    log.warn("SPORT: read at +0x{:05X}", offset);
    return 0xBADACCE5;
}

void Sport::mmio_write32(u32 offset, u32 value) {
    switch (offset & 0xE000) {
        case 0x2000: color_ = value; break;
        case 0x4000: fill_page(offset, value); break;
        default: copy_page(offset, value); break;  // 0x0000 (and, per Opera, anything else)
    }
}

void Sport::copy_page(u32 index, u32 mask) {
    destination_ = page_offset(index);
    if (mask == 0xFFFFFFFF) {
        std::memmove(&vram_[destination_], &vram_[source_], kPageBytes);
        return;
    }
    for (u32 i = 0; i < kPageBytes; i += 4) {
        const u32 src = word(source_ + i);
        set_word(destination_ + i, ((word(destination_ + i) ^ src) & mask) ^ src);
    }
}

void Sport::fill_page(u32 index, u32 mask) {
    const u32 page = page_offset(index);
    for (u32 i = 0; i < kPageBytes; i += 4) {
        const u32 value = mask == 0xFFFFFFFF ? color_ : ((word(page + i) ^ color_) & mask) ^ color_;
        set_word(page + i, value);
    }
}

}  // namespace core::sport
