#include "core/video/vdlp.hpp"

#include <algorithm>

namespace core::video {

namespace {

constexpr u32 kVramMask = 0xFFFFF;  // 1 MB of VRAM

// DMA control word fields.
constexpr u32 persist_len(u32 w) { return w & 0x1FF; }
constexpr u32 ctrl_word_count(u32 w) { return (w >> 9) & 0x3F; }
constexpr bool prev_fba_override(u32 w) { return (w >> 15) & 1; }
constexpr bool curr_fba_override(u32 w) { return (w >> 16) & 1; }
constexpr bool prev_fba_tick(u32 w) { return (w >> 17) & 1; }
constexpr bool next_relative(u32 w) { return (w >> 18) & 1; }
constexpr bool bitmap_dma(u32 w) { return (w >> 21) & 1; }
constexpr u32 modulo_select(u32 w) { return (w >> 23) & 0x7; }

// Display control word fields.
constexpr bool colors_only_bit(u32 w) { return (w >> 1) & 1; }
constexpr bool clut_bypass(u32 w) { return (w >> 25) & 1; }

constexpr std::array<u32, 8> kModuloPixels = {320, 384, 512, 640, 1024, 320, 320, 320};

// First line on which the VDLP starts a new field (Opera).
constexpr u32 kFieldStartLine = 5;

constexpr u32 expand5(u32 v) { return (v << 3) | (v >> 2); }

}  // namespace

void Vdlp::reset() {
    clut_r_ = {};
    clut_g_ = {};
    clut_b_ = {};
    background_ = 0;
    control_ = display_ = 0;
    current_vdl_ = current_fba_ = previous_fba_ = 0;
    lines_left_ = 0;
}

u32 Vdlp::read32(u32 address) const {
    const u32 a = address & kVramMask & ~3u;
    if (a + 4 > vram_.size()) return 0;
    return (u32{vram_[a]} << 24) | (u32{vram_[a + 1]} << 16) | (u32{vram_[a + 2]} << 8) |
           u32{vram_[a + 3]};
}

u16 Vdlp::read16(u32 address) const {
    const u32 a = address & kVramMask & ~1u;
    if (a + 2 > vram_.size()) return 0;
    return static_cast<u16>((vram_[a] << 8) | vram_[a + 1]);
}

u32 Vdlp::modulo_pixels() const {
    return kModuloPixels[modulo_select(control_)];
}

// Frame-buffer lines are stored in pairs: from an even line the next one is
// the other half of the same words (+2), from an odd line it is the start
// of the next pair.
u32 Vdlp::tick(u32 fba) const {
    return fba + ((fba & 2) ? modulo_pixels() * 4 - 2 : 2);
}

void Vdlp::apply_optional_word(u32 word, bool& colors_only) {
    switch (word >> 29) {
        case 0: case 1: case 2: case 3: {  // CLUT colour value word
            const u32 index = (word >> 24) & 0x1F;
            const u8 r = static_cast<u8>(word >> 16), g = static_cast<u8>(word >> 8), b = static_cast<u8>(word);
            switch ((word >> 29) & 0x3) {
                case 0: clut_r_[index] = r; clut_g_[index] = g; clut_b_[index] = b; break;
                case 1: clut_b_[index] = b; break;
                case 2: clut_g_[index] = g; break;
                default: clut_r_[index] = r; break;
            }
            break;
        }
        case 4: case 5: break;  // audio/video output control: not modelled
        case 6:                 // display control
            if (colors_only) break;
            display_ = word;
            colors_only = colors_only_bit(word);
            break;
        default:  // background colour
            background_ = word & 0x00FFFFFF;
            break;
    }
}

void Vdlp::process_entry() {
    const u32 control = read32(current_vdl_);
    if (control == 0) return;
    control_ = control;
    if (curr_fba_override(control)) current_fba_ = read32(current_vdl_ + 4);
    if (prev_fba_override(control)) previous_fba_ = read32(current_vdl_ + 8);
    u32 next = read32(current_vdl_ + 12);
    if (next_relative(control)) next += current_vdl_ + 16;

    bool colors_only = false;
    for (u32 i = 0; i < ctrl_word_count(control); ++i)
        apply_optional_word(read32(current_vdl_ + 16 + 4 * i), colors_only);

    current_vdl_ = next;
    lines_left_ = static_cast<s32>(persist_len(control));
}

void Vdlp::render_line(bool video_dma, std::span<u32> out) const {
    const u32 background = 0xFF000000 | background_;
    if (!video_dma || !bitmap_dma(control_)) {
        std::ranges::fill(out, 0xFF000000);
        return;
    }
    const u32 width = std::min<u32>(modulo_pixels(), static_cast<u32>(out.size()));
    const bool bypass = clut_bypass(display_);
    // Lines narrower than the output (320 in a 384-wide PAL frame) are centred.
    const u32 margin = (static_cast<u32>(out.size()) - width) / 2;
    std::ranges::fill(out, 0xFF000000);
    out = out.subspan(margin, width);
    for (u32 x = 0; x < width; ++x) {
        const u16 p = read16(current_fba_ + 4 * x);
        u32 argb;
        if (p == 0) {
            argb = background;
        } else if (bypass && (p & 0x8000)) {  // fixed CLUT: the pixel is RGB555
            argb = 0xFF000000 | (expand5((p >> 10) & 0x1F) << 16) | (expand5((p >> 5) & 0x1F) << 8) |
                   expand5(p & 0x1F);
        } else {
            argb = 0xFF000000 | (u32{clut_r_[(p >> 10) & 0x1F]} << 16) |
                   (u32{clut_g_[(p >> 5) & 0x1F]} << 8) | u32{clut_b_[p & 0x1F]};
        }
        out[x] = argb;
    }
}

void Vdlp::process_line(u32 line, u32 head, bool clut_dma, bool video_dma, std::span<u32> out) {
    if (line < kFieldStartLine) return;
    if (line == kFieldStartLine) {
        current_vdl_ = head;
        if (clut_dma) process_entry();
    }
    if (clut_dma && lines_left_ <= 0) process_entry();

    if (!out.empty()) render_line(video_dma, out);

    previous_fba_ = prev_fba_tick(control_) ? tick(previous_fba_) : current_fba_;
    current_fba_ = tick(current_fba_);
    if (lines_left_ > 0) --lines_left_;
}

}  // namespace core::video
