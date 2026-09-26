#pragma once

#include <array>
#include <span>

#include "common/types.hpp"

namespace core::video {

// Video Display List Processor: once per scanline, follows the display list
// (VDL) the OS placed in VRAM and turns the current frame-buffer line into
// ARGB8888 through a 32-entry per-channel colour lookup table.
//
// A VDL entry is: DMA control word, current and previous frame-buffer
// addresses (used when the matching override bit is set), next-entry
// pointer, then `ctrl_word_cnt` optional words (CLUT colours, display
// control, background colour). The entry stays in effect for `persist_len`
// lines. Addresses are VRAM offsets (Opera masks them with the VRAM size).
// Behaviour follows Opera's opera_vdlp.c.
class Vdlp {
public:
    explicit Vdlp(std::span<const u8> vram) : vram_(vram) {}

    void reset();

    // Processes scanline `line` of the field. `head` is MADAM's VDL pointer,
    // the flags are MADAM MCTL's CLUT and video DMA enables. When `out` is
    // non-empty the line is visible and its pixels are written there.
    void process_line(u32 line, u32 head, bool clut_dma, bool video_dma, std::span<u32> out);

private:
    [[nodiscard]] u32 read32(u32 address) const;
    [[nodiscard]] u16 read16(u32 address) const;
    void process_entry();
    void apply_optional_word(u32 word, bool& colors_only);
    void render_line(bool video_dma, std::span<u32> out) const;
    [[nodiscard]] u32 modulo_pixels() const;
    [[nodiscard]] u32 tick(u32 fba) const;

    std::span<const u8> vram_;
    std::array<u8, 32> clut_r_{};
    std::array<u8, 32> clut_g_{};
    std::array<u8, 32> clut_b_{};
    u32 background_ = 0;    // 0x00RRGGBB
    u32 control_ = 0;       // current DMA control word
    u32 display_ = 0;       // current display control word
    u32 current_vdl_ = 0;
    u32 current_fba_ = 0;
    u32 previous_fba_ = 0;
    s32 lines_left_ = 0;
};

}  // namespace core::video
