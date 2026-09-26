// Tests for the cel rasteriser, decoder and pixel processor.
#include <algorithm>
#include <vector>

#include "core/madam/cel.hpp"
#include "test_common.hpp"

namespace {

using namespace core::madam;

constexpr int kW = 32;
constexpr int kH = 32;
constexpr u16 kBackground = 0x1234 & 0x7FFF;

struct Canvas {
    std::vector<u16> pixels = std::vector<u16>(kW * kH, kBackground);
    Framebuffer fb() { return {pixels, kW, kH}; }
    u16 at(int x, int y) const { return pixels[static_cast<std::size_t>(y * kW + x)]; }
    int count_changed() const {
        return static_cast<int>(std::ranges::count_if(pixels, [](u16 p) { return p != kBackground; }));
    }
};

u16 rgb(u32 r, u32 g, u32 b, bool p = false) {
    return static_cast<u16>((p ? 0x8000u : 0u) | (r << 10) | (g << 5) | b);
}

// Big-endian 16 bpp rows padded to a word stride.
struct Cel16 {
    std::vector<u8> data;
    u32 pre0 = 0;
    u32 pre1 = 0;

    Cel16(u32 width, u32 height, const std::vector<u16>& pixels) {
        const u32 words_per_row = (width * 2 + 3) / 4;
        const u32 stride = std::max(words_per_row, 2u) * 4;
        data.assign(stride * height, 0);
        for (u32 y = 0; y < height; ++y)
            for (u32 x = 0; x < width; ++x) {
                const u16 p = pixels[y * width + x];
                data[y * stride + x * 2] = static_cast<u8>(p >> 8);
                data[y * stride + x * 2 + 1] = static_cast<u8>(p);
            }
        pre0 = ((height - 1) << pre0::kVCountShift) | pre0::kLinear | pre0::kBpp16;
        pre1 = ((stride / 4 - 2) << pre1::kWOffset10Shift) | (1u << pre1::kTlLsbShift) | (width - 1);
    }

    Ccb ccb(int x, int y) const {
        Ccb c;
        c.x_pos = x << 16;
        c.y_pos = y << 16;
        c.pre0 = pre0;
        c.pre1 = pre1;
        c.source = data;
        return c;
    }
};

void test_identity_blit() {
    const Cel16 cel(2, 2, {rgb(31, 0, 0), rgb(0, 31, 0), rgb(0, 0, 31), rgb(5, 6, 7)});
    Canvas canvas;
    const CelStats stats = draw_cel(cel.ccb(10, 20), canvas.fb());
    CHECK_EQ(stats.texels_drawn, 4u);
    CHECK_EQ(stats.pixels_written, 4u);
    CHECK_EQ(canvas.at(10, 20), rgb(31, 0, 0));
    CHECK_EQ(canvas.at(11, 20), rgb(0, 31, 0));
    CHECK_EQ(canvas.at(10, 21), rgb(0, 0, 31));
    CHECK_EQ(canvas.at(11, 21), rgb(5, 6, 7));
    CHECK_EQ(canvas.count_changed(), 4);
}

void test_transparency_and_bgnd() {
    const Cel16 cel(2, 1, {0x0000, rgb(1, 2, 3)});
    Canvas canvas;
    draw_cel(cel.ccb(0, 0), canvas.fb());
    CHECK_EQ(canvas.at(0, 0), kBackground);  // black source pixel skipped
    CHECK_EQ(canvas.at(1, 0), rgb(1, 2, 3));

    Ccb bgnd = cel.ccb(0, 1);
    bgnd.flags |= ccb_flag::kBgnd;
    draw_cel(bgnd, canvas.fb());
    CHECK_EQ(canvas.at(0, 1), 0x0400u);      // drawn, but black output becomes near-black

    bgnd.flags |= ccb_flag::kNoBlk;
    bgnd.y_pos = 2 << 16;
    draw_cel(bgnd, canvas.fb());
    CHECK_EQ(canvas.at(0, 2), 0x0000u);      // NOBLK allows true black
}

void test_scaled_texel_covers_block() {
    const Cel16 cel(1, 1, {rgb(9, 9, 9)});
    Ccb ccb = cel.ccb(4, 4);
    ccb.hdx = 3 << 20;  // 12.20
    ccb.vdy = 2 << 16;  // 16.16
    Canvas canvas;
    const CelStats stats = draw_cel(ccb, canvas.fb());
    CHECK_EQ(stats.pixels_written, 6u);
    CHECK_EQ(canvas.at(4, 4), rgb(9, 9, 9));
    CHECK_EQ(canvas.at(6, 5), rgb(9, 9, 9));
    CHECK_EQ(canvas.at(7, 4), kBackground);  // right edge excluded
    CHECK_EQ(canvas.at(4, 6), kBackground);  // bottom edge excluded
}

void test_rotated_cel_has_no_gaps_or_overlaps() {
    std::vector<u16> pixels(8 * 8, rgb(20, 10, 5));
    const Cel16 cel(8, 8, pixels);
    Ccb ccb = cel.ccb(16, 4);
    // 45 degrees, scale ~1.5: H = (1.06, 1.06), V = (-1.06, 1.06).
    ccb.hdx = ccb.hdy = static_cast<s32>(1.06 * (1 << 20));
    ccb.vdx = -static_cast<s32>(1.06 * (1 << 16));
    ccb.vdy = static_cast<s32>(1.06 * (1 << 16));
    ccb.pixc = (u32{kPpmpAverage} << 16) | kPpmpAverage;  // a double write would show
    Canvas canvas;
    const CelStats stats = draw_cel(ccb, canvas.fb());
    CHECK_EQ(stats.texels_drawn, 64u);
    CHECK_EQ(static_cast<int>(stats.pixels_written), canvas.count_changed());  // each pixel once
    CHECK_EQ(canvas.at(16, 10), (u16)((((20 + 4) >> 1) << 10) | (((10 + 17) >> 1) << 5) | ((5 + 20) >> 1)));
}

void test_warp_widens_rows() {
    std::vector<u16> pixels(4 * 4, rgb(3, 3, 3));
    const Cel16 cel(4, 4, pixels);
    Ccb ccb = cel.ccb(2, 2);
    ccb.hddx = 1 << 20;  // each row's H grows by one pixel: a trapezoid
    Canvas canvas;
    draw_cel(ccb, canvas.fb());
    auto row_width = [&](int y) {
        int n = 0;
        for (int x = 0; x < kW; ++x) n += canvas.at(x, y) != kBackground;
        return n;
    };
    // Pixels sample near their bottom edge, where each row is almost as wide
    // as the next row's H: 4 texels x (H just under 2, 3, 5).
    CHECK_EQ(row_width(2), 7);
    CHECK_EQ(row_width(3), 11);
    CHECK_EQ(row_width(5), 19);
    CHECK_EQ(row_width(6), 0);
}

void test_half_pixel_position() {
    // A cel at X = 10.5 starts on pixel 10, as in Opera (corners are floored).
    const Cel16 cel(2, 1, {rgb(1, 2, 3), rgb(4, 5, 6)});
    Ccb ccb = cel.ccb(0, 5);
    ccb.x_pos = (10 << 16) | 0x8000;
    ccb.y_pos = (5 << 16) | 0x8000;
    Canvas canvas;
    draw_cel(ccb, canvas.fb());
    CHECK_EQ(canvas.at(10, 5), rgb(1, 2, 3));
    CHECK_EQ(canvas.at(11, 5), rgb(4, 5, 6));
    CHECK_EQ(canvas.at(12, 5), kBackground);
    CHECK_EQ(canvas.at(10, 6), kBackground);
    CHECK_EQ(canvas.count_changed(), 2);
}

void test_face_culling() {
    const Cel16 cel(2, 2, {rgb(1, 1, 1), rgb(1, 1, 1), rgb(1, 1, 1), rgb(1, 1, 1)});
    Ccb mirrored = cel.ccb(10, 10);
    mirrored.hdx = -(1 << 20);  // H points left: counter-clockwise texels

    Canvas canvas;
    mirrored.flags = ccb_flag::kAcw;
    CHECK_EQ(draw_cel(mirrored, canvas.fb()).pixels_written, 0u);
    mirrored.flags = ccb_flag::kAccw;
    CHECK_EQ(draw_cel(mirrored, canvas.fb()).pixels_written, 4u);
    CHECK_EQ(canvas.at(9, 10), rgb(1, 1, 1));
    mirrored.flags = 0;
    CHECK_EQ(draw_cel(mirrored, canvas.fb()).pixels_written, 0u);
}

void test_coded_4bpp_with_pluta() {
    // One row of four 4-bit pixels: 1, 2, 0, 15.
    const std::vector<u8> data = {0x12, 0x0F, 0, 0, 0, 0, 0, 0};
    Ccb ccb;
    ccb.pre0 = pre0::kBpp4;  // 1 row, coded
    ccb.pre1 = (0u << pre1::kWOffset8Shift) | (1u << pre1::kTlLsbShift) | 3u;
    ccb.source = data;
    for (u16 i = 0; i < 32; ++i) ccb.plut[i] = rgb(i, 0, 0);
    ccb.plut[16] = 0;  // index 0 with PLUTA bit 3 -> transparent black

    Canvas canvas;
    draw_cel(ccb, canvas.fb());
    CHECK_EQ(canvas.at(0, 0), rgb(1, 0, 0));
    CHECK_EQ(canvas.at(1, 0), rgb(2, 0, 0));
    CHECK_EQ(canvas.at(3, 0), rgb(15, 0, 0));

    ccb.flags |= 0x8;  // PLUTA bit 3: indices move to the upper 16 entries
    ccb.y_pos = 1 << 16;
    draw_cel(ccb, canvas.fb());
    CHECK_EQ(canvas.at(0, 1), rgb(17, 0, 0));
    CHECK_EQ(canvas.at(2, 1), kBackground);  // PLUT[16] is black -> transparent
    CHECK_EQ(canvas.at(3, 1), rgb(31, 0, 0));
}

void test_packed_cel() {
    // One packed 16 bpp row: repeat x3, transparent x1, literal x1, end of row.
    const std::vector<u8> data = {0x00, 0x01,              // offset: next row 3 words on
                                  0xC2, 0x03, 0xE0,        // repeat 3 x 0x03E0
                                  0x80,                    // 1 transparent
                                  0x40, 0x00, 0x1F,        // literal 0x001F
                                  0x00, 0x00, 0x00};       // end of row
    Ccb ccb;
    ccb.flags |= ccb_flag::kPacked;
    ccb.pre0 = pre0::kBpp16 | pre0::kLinear;  // one row
    ccb.source = data;

    Canvas canvas;
    const CelStats stats = draw_cel(ccb, canvas.fb());
    CHECK_EQ(stats.rows, 1u);
    CHECK_EQ(canvas.at(0, 0), 0x03E0u);
    CHECK_EQ(canvas.at(2, 0), 0x03E0u);
    CHECK_EQ(canvas.at(3, 0), kBackground);   // transparent packet
    CHECK_EQ(canvas.at(4, 0), 0x001Eu);       // packed cels: blue LSB from CECONTROL (0)
    CHECK_EQ(canvas.at(5, 0), kBackground);
}

void test_vram_line_pair_layout() {
    std::vector<u8> ram(0x10000);
    const Cel16 cel(1, 2, {rgb(31, 0, 0), rgb(0, 31, 0)});  // two rows
    Ccb ccb = cel.ccb(3, 4);
    VramTarget target{ram, 0x1000, 0x1000, 320 * 4, 320 * 4, 319, 239};
    draw_cel(ccb, target);
    // (3,4): pair 2, even half; (3,5): same word, odd half.
    const std::size_t even = 0x1000 + 2 * 320 * 4 + 3 * 4;
    CHECK_EQ((ram[even] << 8) | ram[even + 1], rgb(31, 0, 0));
    CHECK_EQ((ram[even + 2] << 8) | ram[even + 3], rgb(0, 31, 0));
}

void test_lrform_cel() {
    // A 2x2 LRFORM source: one line pair, both pixels of a column in one word.
    std::vector<u8> ram(0x100);
    const u16 px[2][2] = {{rgb(31, 0, 0), rgb(0, 31, 0)}, {rgb(0, 0, 31), rgb(5, 5, 5)}};  // [row][col]
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y) {
            const std::size_t a = static_cast<std::size_t>(x * 4 + y * 2);
            ram[a] = static_cast<u8>(px[y][x] >> 8);
            ram[a + 1] = static_cast<u8>(px[y][x]);
        }
    Ccb ccb;
    ccb.pre0 = pre0::kBpp16 | pre0::kLinear;  // VCNT 0: one line pair = 2 rows
    ccb.pre1 = pre1::kLrForm | (0u << pre1::kWOffset10Shift) | (1u << pre1::kTlLsbShift) | 1u;  // 2 wide
    ccb.source = ram;
    ccb.x_pos = 3 << 16;
    ccb.y_pos = 4 << 16;
    Canvas canvas;
    const CelStats stats = draw_cel(ccb, canvas.fb());
    CHECK_EQ(stats.rows, 2u);
    CHECK_EQ(canvas.at(3, 4), rgb(31, 0, 0));
    CHECK_EQ(canvas.at(4, 4), rgb(0, 31, 0));
    CHECK_EQ(canvas.at(3, 5), rgb(0, 0, 31));
    CHECK_EQ(canvas.at(4, 5), rgb(5, 5, 5));
}

void test_pixel_processor() {
    const u32 normal_avg = (u32{kPpmpAverage} << 16) | kPpmpNormal;
    const u16 frame = rgb(10, 20, 30);

    // P = 0 selects the low half (normal), P = 1 the high half (average).
    CHECK_EQ(process_pixel(0, normal_avg, rgb(2, 4, 6), frame, 0x49), rgb(2, 4, 6));
    CHECK_EQ(process_pixel(0, normal_avg, rgb(2, 4, 6, true), frame, 0x49), rgb(6, 12, 18));

    // POVER = 3 forces P to 1, POVER = 2 forces it to 0.
    CHECK_EQ(process_pixel(3u << 7, normal_avg, rgb(2, 4, 6), frame, 0x49), rgb(6, 12, 18));
    CHECK_EQ(process_pixel(2u << 7, normal_avg, rgb(2, 4, 6, true), frame, 0x49), rgb(2, 4, 6));

    // Additive: source*8/8 + frame, clamped to 31.
    const u16 additive = 0x1F80;  // MF=8, DV1=/8, S2=frame, DV2=/1
    CHECK_EQ(process_pixel(0, additive, rgb(25, 4, 1), frame, 0x49), rgb(31, 24, 31));

    // Subtractive with USEAV: frame - source (S1 = frame, S2 = cel, NEG).
    const u16 subtract = 0x8000 | 0x1F00 | 0xC0 | (0x01 << 1);
    CHECK_EQ(process_pixel(ccb_flag::kUseAv, subtract, rgb(4, 30, 1), frame, 0x49),
             rgb(6, 0, 29));

    // Half-brightness: MF=4, DV1=/8.
    const u16 half = 0x0C00 | 0x0300 | 0x40;
    CHECK_EQ(process_pixel(0, half, rgb(30, 16, 2), frame, 0x49), rgb(15, 8, 1));
}

}  // namespace

int main() {
    test_identity_blit();
    test_transparency_and_bgnd();
    test_scaled_texel_covers_block();
    test_rotated_cel_has_no_gaps_or_overlaps();
    test_warp_widens_rows();
    test_face_culling();
    test_half_pixel_position();
    test_coded_4bpp_with_pluta();
    test_pixel_processor();
    test_packed_cel();
    test_lrform_cel();
    test_vram_line_pair_layout();
    return report("cel_test");
}
