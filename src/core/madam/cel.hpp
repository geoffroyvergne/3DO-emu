#pragma once

#include <array>
#include <span>

#include "common/types.hpp"

namespace core::madam {

// Bit definitions below are from Portfolio OS `hardware.h`; rendering behaviour
// is cross-checked against the Opera (libretro) cel engine.

// CCB flags word.
namespace ccb_flag {
inline constexpr u32 kSkip = 0x80000000;
inline constexpr u32 kLast = 0x40000000;
inline constexpr u32 kNpAbs = 0x20000000;
inline constexpr u32 kSpAbs = 0x10000000;
inline constexpr u32 kPpAbs = 0x08000000;
inline constexpr u32 kLdSize = 0x04000000;  // CCB carries HDX/HDY/VDX/VDY
inline constexpr u32 kLdPrs = 0x02000000;   // CCB carries HDDX/HDDY
inline constexpr u32 kLdPpmp = 0x01000000;  // CCB carries PIXC
inline constexpr u32 kLdPlut = 0x00800000;
inline constexpr u32 kCcbPre = 0x00400000;  // preamble in the CCB instead of the source data
inline constexpr u32 kYoxy = 0x00200000;
inline constexpr u32 kAcsc = 0x00100000;
inline constexpr u32 kAlsc = 0x00080000;
inline constexpr u32 kAcw = 0x00040000;     // draw clockwise-facing texels
inline constexpr u32 kAccw = 0x00020000;    // draw counter-clockwise-facing texels
inline constexpr u32 kTwd = 0x00010000;
inline constexpr u32 kLce = 0x00008000;
inline constexpr u32 kAce = 0x00004000;
inline constexpr u32 kMaria = 0x00001000;
inline constexpr u32 kPxor = 0x00000800;    // pixel processor XORs instead of adds
inline constexpr u32 kUseAv = 0x00000400;   // PIXC AV field drives the adder options
inline constexpr u32 kPacked = 0x00000200;
inline constexpr u32 kPoverMask = 0x00000180;
inline constexpr u32 kPoverShift = 7;
inline constexpr u32 kPlutPos = 0x00000040;
inline constexpr u32 kBgnd = 0x00000020;    // black source pixels are drawn, not transparent
inline constexpr u32 kNoBlk = 0x00000010;   // allow pure black output (else becomes 0x0400)
inline constexpr u32 kPlutaMask = 0x0000000F;
}  // namespace ccb_flag

// First preamble word.
namespace pre0 {
inline constexpr u32 kLiteral = 0x80000000;
inline constexpr u32 kBgnd = 0x40000000;
inline constexpr u32 kSkipXMask = 0x0F000000;
inline constexpr u32 kSkipXShift = 24;
inline constexpr u32 kVCountMask = 0x0000FFC0;  // rows - 1
inline constexpr u32 kVCountShift = 6;
inline constexpr u32 kLinear = 0x00000010;      // uncoded: pixels are colours, not PLUT indices
inline constexpr u32 kRep8 = 0x00000008;
inline constexpr u32 kBppMask = 0x00000007;
inline constexpr u32 kBpp1 = 1, kBpp2 = 2, kBpp4 = 3, kBpp6 = 4, kBpp8 = 5, kBpp16 = 6;
}  // namespace pre0

// Second preamble word (unpacked cels only).
namespace pre1 {
inline constexpr u32 kWOffset8Mask = 0xFF000000;  // row stride in words - 2, for < 8 bpp
inline constexpr u32 kWOffset8Shift = 24;
inline constexpr u32 kWOffset10Mask = 0x03FF0000;  // row stride in words - 2, for >= 8 bpp
inline constexpr u32 kWOffset10Shift = 16;
inline constexpr u32 kNoSwap = 0x00004000;
inline constexpr u32 kTlLsbMask = 0x00003000;  // source of the output blue LSB
inline constexpr u32 kTlLsbShift = 12;
inline constexpr u32 kLrForm = 0x00000800;
inline constexpr u32 kTlHpCountMask = 0x000007FF;  // pixels per row - 1
}  // namespace pre1

// MADAM CECONTROL register (cel engine control), from Opera's opera_madam.c.
namespace cecontrol {
inline constexpr u32 kB15PosMask = 0xC0000000;  // output bit 15: 0, 1, or decoder/origin (PDC)
inline constexpr u32 kB15Pos0 = 0x00000000;
inline constexpr u32 kB15Pos1 = 0x40000000;
inline constexpr u32 kB15PosPdc = 0xC0000000;
inline constexpr u32 kB0PosMask = 0x30000000;   // output bit 0: 0, 1, pixel processor, or PDC
inline constexpr u32 kB0Pos0 = 0x00000000;
inline constexpr u32 kB0Pos1 = 0x10000000;
inline constexpr u32 kB0PosPixc = 0x20000000;
inline constexpr u32 kB0PosPdc = 0x30000000;
inline constexpr u32 kSwapHv = 0x08000000;
inline constexpr u32 kPdcLsbMask = 0x00300000;  // blue LSB source for packed cels
inline constexpr u32 kPdcLsbShift = 20;
}  // namespace cecontrol

// PIXC halves (PPMP) for the two common modes, from Portfolio `graphics.h`.
inline constexpr u16 kPpmpNormal = 0x1F40;   // output = source
inline constexpr u16 kPpmpAverage = 0x1F81;  // output = (source + frame) / 2

// A cel control block after it has been fetched from memory: every optional
// field is resolved, and the preamble and PLUT are in host order.
// Fixed-point formats are the hardware's: positions and VDX/VDY are 16.16,
// HDX/HDY/HDDX/HDDY are 12.20.
struct Ccb {
    u32 flags = ccb_flag::kAcw | ccb_flag::kAccw;
    s32 x_pos = 0;
    s32 y_pos = 0;
    s32 hdx = 1 << 20;  // one pixel right per source column
    s32 hdy = 0;
    s32 vdx = 0;
    s32 vdy = 1 << 16;  // one pixel down per source row
    s32 hddx = 0;       // added to HDX/HDY after every row: this is the warp
    s32 hddy = 0;
    u32 pixc = (u32{kPpmpNormal} << 16) | kPpmpNormal;
    u32 pre0 = 0;
    u32 pre1 = 0;               // unpacked cels only
    std::span<const u8> source;  // big-endian pixel rows (after any in-data preamble)
    std::array<u16, 32> plut{};
    // Cel engine control (a MADAM register, not part of the CCB). The default
    // keeps the pixel processor's bit 0 and the decoder/origin bit 15.
    u32 cecontrol = cecontrol::kB15PosPdc | cecontrol::kB0PosPixc;
};

// Row-major host-order bitmap (tests and the no-BIOS demo).
struct Framebuffer {
    std::span<u16> pixels;
    int width = 0;
    int height = 0;
};

// The real 3DO frame buffer in RAM: big-endian 16-bit pixels stored in
// line pairs, offset(x, y) = (y / 2) * modulo + (y & 1) * 2 + x * 4
// (Opera's XY2OFF). Reads (for blending) and writes may use different
// buffers, as MADAM's REGCTL2/REGCTL3 allow.
struct VramTarget {
    std::span<u8> ram;       // whole DRAM+VRAM block; bases are addresses in it
    u32 read_base = 0;       // REGCTL2
    u32 write_base = 0;      // REGCTL3
    u32 read_modulo = 0;     // bytes per line pair, from REGCTL0
    u32 write_modulo = 0;
    u32 clip_x = 0;          // last drawable column / row (REGCTL1)
    u32 clip_y = 0;
};

struct CelStats {
    u32 texels_drawn = 0;
    u32 pixels_written = 0;
    u32 rows = 0;  // source rows walked; the engine advances XY by VD * rows
};

// Draws one cel (packed or unpacked) as a forward-mapped, warped quad.
CelStats draw_cel(const Ccb& ccb, Framebuffer target);
CelStats draw_cel(const Ccb& ccb, const VramTarget& target);

// Pixel processor: combines a decoded source pixel with the frame buffer
// pixel according to the PIXC half selected by the source's P-mode bit.
u16 process_pixel(u32 ccb_flags, u32 pixc, u16 source, u16 frame, u16 amv);

}  // namespace core::madam
