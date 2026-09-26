#include "core/madam/cel.hpp"

#include <algorithm>
#include <vector>

#include "common/endian.hpp"
#include "common/log.hpp"

namespace core::madam {

namespace {

constexpr std::array<u32, 8> kBitsPerPixel = {0, 1, 2, 4, 6, 8, 16, 0};

// Multiplier selection used by PIXC "MS = PIN" for formats that carry no AMV
// bits of their own (value taken from Opera's decoder).
constexpr u16 kDefaultAmv = 0x49;

constexpr u16 kNearBlack = 0x0400;  // RGB555 (1,0,0)

// Longest row a packed cel may produce before we assume corrupt data.
constexpr std::size_t kMaxPackedRowPixels = 2048;

constexpr u32 channel(u16 pixel, u32 index) {  // 0 = red, 1 = green, 2 = blue
    return (pixel >> (10 - 5 * index)) & 0x1F;
}

// PIXC divider field: 1 -> /2, 2 -> /4, 3 -> /8, 0 -> /16.
constexpr u32 divider_shift(u32 field) {
    return ((field - 1) & 3) + 1;
}

// MSB-first reader over big-endian cel data. Reads past the end return zeros.
class BitReader {
public:
    BitReader(std::span<const u8> data, std::size_t byte_offset)
        : data_(data), bit_(byte_offset * 8) {}

    void skip(u32 bits) { bit_ += bits; }
    [[nodiscard]] std::size_t byte_position() const { return bit_ / 8; }

    u32 read(u32 bits) {
        u32 value = 0;
        for (u32 i = 0; i < bits; ++i, ++bit_) {
            const std::size_t byte = bit_ / 8;
            const u32 bit = byte < data_.size() ? (data_[byte] >> (7 - bit_ % 8)) & 1u : 0u;
            value = (value << 1) | bit;
        }
        return value;
    }

private:
    std::span<const u8> data_;
    std::size_t bit_;
};

// --- Pixel decoder -------------------------------------------------------------

struct Texel {
    u16 value;         // decoded colour, bit 15 = P-mode, before blue-LSB substitution
    u16 amv;           // per-channel multipliers for PIXC "MS = PIN"
    bool transparent;
};

bool format_supported(u32 bpp_code) {
    return bpp_code >= pre0::kBpp1 && bpp_code <= pre0::kBpp16;
}

// 8-bit uncoded r3 g3 b2 -> RGB555 (Opera's MAPu8b table).
u16 expand_uncoded8(u32 raw) {
    const u32 b = raw & 0x3, g = (raw >> 2) & 0x7, r = (raw >> 5) & 0x7;
    const u32 b5 = (b << 3) + (b << 1) + (b >> 1);
    const u32 g5 = (g << 2) + (g >> 1);
    const u32 r5 = (r << 2) + (r >> 1);
    return static_cast<u16>((r5 << 10) | (g5 << 5) | b5);
}

Texel decode_pixel(const Ccb& ccb, u32 bpp_code, u32 raw) {
    const u32 pluta = ccb.flags & ccb_flag::kPlutaMask;
    const bool linear = (ccb.pre0 & pre0::kLinear) != 0;
    u16 value = 0;
    u16 amv = kDefaultAmv;

    switch (bpp_code) {
        // Low-depth coded pixels take their upper PLUT index bits from PLUTA.
        case pre0::kBpp1: value = ccb.plut[(pluta & 0xF) * 2 + raw]; break;
        case pre0::kBpp2: value = ccb.plut[(pluta & 0xE) * 2 + raw]; break;
        case pre0::kBpp4: value = ccb.plut[(pluta & 0x8) * 2 + raw]; break;
        case pre0::kBpp6:  // 5-bit PLUT index plus the P-mode bit
            value = static_cast<u16>((ccb.plut[raw & 0x1F] & 0x7FFF) | ((raw & 0x20) << 10));
            break;
        case pre0::kBpp8:
            if (linear) {
                value = expand_uncoded8(raw);
            } else {  // c:5, P:1, m:2 -> same multiplier for all channels
                value = ccb.plut[raw & 0x1F];
                const u32 m = (((raw >> 6) & 0x3) << 1) | ((raw >> 5) & 1);
                amv = static_cast<u16>((m << 6) | (m << 3) | m);
            }
            break;
        default:  // 16 bpp
            if (linear) {
                value = static_cast<u16>(raw);
            } else {  // c:5, mb:3, mg:3, mr:3, pad, P
                value = static_cast<u16>((ccb.plut[raw & 0x1F] & 0x7FFF) | (raw & 0x8000));
                const u32 mb = (raw >> 5) & 7, mg = (raw >> 8) & 7, mr = (raw >> 11) & 7;
                amv = static_cast<u16>((mr << 6) | (mg << 3) | mb);
            }
            break;
    }
    // Transparency: decoded colour black, unless the CCB says BGND.
    const bool transparent = !(ccb.flags & ccb_flag::kBgnd) && (value & 0x7FFF) == 0;
    return {value, amv, transparent};
}

// PRE1 TLLSB / CECONTROL PDCLSB: where the output blue LSB comes from.
u16 substitute_blue_lsb(u16 pixel, u32 mode) {
    u32 lsb = 0;
    switch (mode) {
        case 1: lsb = pixel & 1u; break;          // keep (normal)
        case 2: lsb = (pixel >> 4) & 1u; break;   // blue MSB
        case 3: lsb = (pixel >> 5) & 1u; break;   // green LSB
        default: break;                           // zero
    }
    return static_cast<u16>((pixel & ~1u) | lsb);
}

// Projector output stage: sets bits 15 and 0 of the written pixel
// (Opera's PPROJ_OUTPUT).
u16 project(const Ccb& ccb, u16 processed, u16 decoded) {
    u16 vh = (ccb.flags & ccb_flag::kPlutPos)
                 ? static_cast<u16>(decoded & 0x8001)
                 : static_cast<u16>((ccb.x_pos & 1) | ((ccb.y_pos & 1) << 15));
    if ((ccb.cecontrol & cecontrol::kSwapHv) && !(ccb.pre1 & pre1::kNoSwap))
        vh = static_cast<u16>((vh >> 15) | ((vh & 1) << 15));

    switch (ccb.cecontrol & cecontrol::kB15PosMask) {
        case cecontrol::kB15Pos0: vh &= 0x7FFF; break;
        case cecontrol::kB15Pos1: vh |= 0x8000; break;
        default: break;
    }
    switch (ccb.cecontrol & cecontrol::kB0PosMask) {
        case cecontrol::kB0Pos0: vh &= static_cast<u16>(~1u); break;
        case cecontrol::kB0Pos1: vh |= 1; break;
        case cecontrol::kB0PosPixc: vh = static_cast<u16>((vh & ~1u) | (processed & 1u)); break;
        default: break;
    }
    return static_cast<u16>((processed & 0x7FFE) | vh);
}

// --- Row sources -------------------------------------------------------------------

// Unpacked rows: fixed width and stride.
class LiteralRows {
public:
    LiteralRows(const Ccb& ccb, u32 bpp_code) : ccb_(ccb), bpp_code_(bpp_code) {
        bpp_ = kBitsPerPixel[bpp_code];
        const u32 word_offset = bpp_ < 8 ? (ccb.pre1 & pre1::kWOffset8Mask) >> pre1::kWOffset8Shift
                                         : (ccb.pre1 & pre1::kWOffset10Mask) >> pre1::kWOffset10Shift;
        row_bytes_ = (std::size_t{word_offset} + 2) * 4;
        width_ = (ccb.pre1 & pre1::kTlHpCountMask) + 1;
        skip_x_ = (ccb.pre0 & pre0::kSkipXMask) >> pre0::kSkipXShift;
        rows_ = ((ccb.pre0 & pre0::kVCountMask) >> pre0::kVCountShift) + 1;
    }

    [[nodiscard]] u32 rows() const { return rows_; }

    void decode_row(u32 row, std::vector<Texel>& out) {
        out.clear();
        if (skip_x_ >= width_) return;
        BitReader bits(ccb_.source, row * row_bytes_);
        bits.skip(skip_x_ * bpp_);
        for (u32 x = skip_x_; x < width_; ++x) out.push_back(decode_pixel(ccb_, bpp_code_, bits.read(bpp_)));
    }

private:
    const Ccb& ccb_;
    u32 bpp_code_;
    u32 bpp_ = 0;
    std::size_t row_bytes_ = 0;
    u32 width_ = 0;
    u32 skip_x_ = 0;
    u32 rows_ = 0;
};

// Packed rows: each starts with a word offset to the next row, then packets
// of a 2-bit type and a 6-bit count-1: 0 end of row, 1 literal pixels,
// 2 transparent run, 3 one pixel repeated (Opera's DrawPackedCel).
class PackedRows {
public:
    PackedRows(const Ccb& ccb, u32 bpp_code) : ccb_(ccb), bpp_code_(bpp_code) {
        bpp_ = kBitsPerPixel[bpp_code];
        offset_bits_ = bpp_ < 8 ? 8 : 16;
        skip_x_ = (ccb.pre0 & pre0::kSkipXMask) >> pre0::kSkipXShift;
        rows_ = ((ccb.pre0 & pre0::kVCountMask) >> pre0::kVCountShift) + 1;
    }

    [[nodiscard]] u32 rows() const { return rows_; }

    // Rows must be decoded in order: each one locates the next.
    void decode_row(u32 /*row*/, std::vector<Texel>& out) {
        out.clear();
        BitReader bits(ccb_.source, row_start_);
        const std::size_t next_row = row_start_ + (std::size_t{bits.read(offset_bits_)} + 2) * 4;
        u32 skip = skip_x_;

        auto emit = [&](const Texel& t) {
            if (skip > 0) {
                --skip;
                return;
            }
            out.push_back(t);
        };

        while (out.size() < kMaxPackedRowPixels) {
            const u32 type = bits.read(2);
            if (bits.byte_position() >= next_row || type == 0) break;
            const u32 count = bits.read(6) + 1;
            switch (type) {
                case 1:  // literal
                    for (u32 i = 0; i < count; ++i) emit(decode_pixel(ccb_, bpp_code_, bits.read(bpp_)));
                    break;
                case 2:  // transparent run
                    for (u32 i = 0; i < count; ++i) emit({0, kDefaultAmv, true});
                    break;
                default: {  // repeated pixel
                    const Texel t = decode_pixel(ccb_, bpp_code_, bits.read(bpp_));
                    for (u32 i = 0; i < count; ++i) emit(t);
                    break;
                }
            }
        }
        row_start_ = next_row;
    }

private:
    const Ccb& ccb_;
    u32 bpp_code_;
    u32 bpp_ = 0;
    u32 offset_bits_ = 8;
    u32 skip_x_ = 0;
    u32 rows_ = 0;
    std::size_t row_start_ = 0;
};

// --- Targets -----------------------------------------------------------------------

class LinearSurface {
public:
    explicit LinearSurface(Framebuffer fb) : fb_(fb) {}
    [[nodiscard]] int width() const { return fb_.width; }
    [[nodiscard]] int height() const { return fb_.height; }
    [[nodiscard]] u16 read(int x, int y) const { return fb_.pixels[index(x, y)]; }
    void write(int x, int y, u16 v) { fb_.pixels[index(x, y)] = v; }

private:
    [[nodiscard]] std::size_t index(int x, int y) const {
        return static_cast<std::size_t>(y * fb_.width + x);
    }
    Framebuffer fb_;
};

class VramSurface {
public:
    explicit VramSurface(const VramTarget& t) : t_(t) {}
    [[nodiscard]] int width() const { return static_cast<int>(t_.clip_x) + 1; }
    [[nodiscard]] int height() const { return static_cast<int>(t_.clip_y) + 1; }
    [[nodiscard]] u16 read(int x, int y) const {
        const std::size_t a = address(t_.read_base, t_.read_modulo, x, y);
        return a + 1 < t_.ram.size() ? static_cast<u16>((t_.ram[a] << 8) | t_.ram[a + 1]) : 0;
    }
    void write(int x, int y, u16 v) {
        const std::size_t a = address(t_.write_base, t_.write_modulo, x, y);
        if (a + 1 >= t_.ram.size()) return;
        t_.ram[a] = static_cast<u8>(v >> 8);
        t_.ram[a + 1] = static_cast<u8>(v);
    }

private:
    static std::size_t address(u32 base, u32 modulo, int x, int y) {
        const auto ux = static_cast<u32>(x), uy = static_cast<u32>(y);
        return std::size_t{base} + (uy >> 1) * modulo + (uy & 1) * 2 + ux * 4;
    }
    VramTarget t_;
};

// --- Rasteriser --------------------------------------------------------------------
// Coordinates are 16.16. Pixels are sampled at integer positions (as Opera
// does), and each texel quad is split into two triangles filled with a
// top-left rule, so adjacent texels neither overlap nor leave gaps.

struct Point {
    s64 x;
    s64 y;
};

constexpr s64 floor_to_int(s64 v) { return v >> 16; }
constexpr s64 ceil_to_int(s64 v) { return (v + 0xFFFF) >> 16; }

constexpr s64 edge(Point a, Point b, Point p) {
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

// For triangles with positive edge() orientation (clockwise on a y-down screen).
constexpr bool is_top_left(Point a, Point b) {
    return (a.y == b.y && b.x > a.x) || b.y < a.y;
}

template <typename Plot>
void fill_triangle(Point a, Point b, Point c, int width, int height, Plot&& plot) {
    if (edge(a, b, c) < 0) std::swap(b, c);
    if (edge(a, b, c) == 0) return;

    const s64 min_x = std::max<s64>(ceil_to_int(std::min({a.x, b.x, c.x})), 0);
    const s64 max_x = std::min<s64>(floor_to_int(std::max({a.x, b.x, c.x})), width - 1);
    const s64 min_y = std::max<s64>(ceil_to_int(std::min({a.y, b.y, c.y})), 0);
    const s64 max_y = std::min<s64>(floor_to_int(std::max({a.y, b.y, c.y})), height - 1);

    const bool tl_ab = is_top_left(a, b);
    const bool tl_bc = is_top_left(b, c);
    const bool tl_ca = is_top_left(c, a);

    for (s64 y = min_y; y <= max_y; ++y) {
        for (s64 x = min_x; x <= max_x; ++x) {
            const Point p{x << 16, y << 16};
            const s64 e0 = edge(a, b, p);
            const s64 e1 = edge(b, c, p);
            const s64 e2 = edge(c, a, p);
            if ((e0 > 0 || (e0 == 0 && tl_ab)) && (e1 > 0 || (e1 == 0 && tl_bc)) &&
                (e2 > 0 || (e2 == 0 && tl_ca)))
                plot(static_cast<int>(x), static_cast<int>(y));
        }
    }
}

Log::Limiter format_log("Cel engine: unimplemented source format", 4);

template <typename Rows, typename Surface>
CelStats rasterise(const Ccb& ccb, Rows& rows, Surface& surface, u32 blue_lsb_mode) {
    CelStats stats;
    stats.rows = rows.rows();
    // With neither face enabled nothing is drawn (Opera behaviour).
    if (!(ccb.flags & (ccb_flag::kAcw | ccb_flag::kAccw))) return stats;

    // Everything in 16.16; the 12.20 horizontal terms lose their 4 extra bits.
    s64 hdx = ccb.hdx >> 4;
    s64 hdy = ccb.hdy >> 4;
    const s64 hddx = ccb.hddx >> 4;
    const s64 hddy = ccb.hddy >> 4;
    Point row_start{ccb.x_pos, ccb.y_pos};
    std::vector<Texel> texels;

    for (u32 row = 0; row < rows.rows(); ++row) {
        // The next row's corners use the H vector after this row's HDD step.
        const s64 next_hdx = hdx + hddx;
        const s64 next_hdy = hdy + hddy;
        const Point next_row_start{row_start.x + ccb.vdx, row_start.y + ccb.vdy};
        rows.decode_row(row, texels);

        for (std::size_t col = 0; col < texels.size(); ++col) {
            const Texel& texel = texels[col];
            if (texel.transparent) continue;

            const auto c = static_cast<s64>(col);
            const Point a{row_start.x + c * hdx, row_start.y + c * hdy};
            const Point b{a.x + hdx, a.y + hdy};
            const Point d{next_row_start.x + c * next_hdx, next_row_start.y + c * next_hdy};
            const Point e{d.x + next_hdx, d.y + next_hdy};

            // Face culling: H then V turning clockwise on screen (the unrotated
            // case) is drawn when ACW is set, the mirror image needs ACCW.
            const s64 facing = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x);
            if (facing == 0) continue;
            if (!(ccb.flags & (facing > 0 ? ccb_flag::kAcw : ccb_flag::kAccw))) continue;

            const u16 source = substitute_blue_lsb(texel.value, blue_lsb_mode);
            auto plot = [&](int x, int y) {
                const u16 processed =
                    process_pixel(ccb.flags, ccb.pixc, source, surface.read(x, y), texel.amv);
                surface.write(x, y, project(ccb, processed, texel.value));
                ++stats.pixels_written;
            };
            fill_triangle(a, b, e, surface.width(), surface.height(), plot);
            fill_triangle(a, e, d, surface.width(), surface.height(), plot);
            ++stats.texels_drawn;
        }

        hdx = next_hdx;
        hdy = next_hdy;
        row_start = next_row_start;
    }
    return stats;
}

template <typename Surface>
CelStats draw(const Ccb& ccb, Surface& surface) {
    if (ccb.flags & ccb_flag::kSkip) return {};

    const u32 bpp_code = ccb.pre0 & pre0::kBppMask;
    const bool packed = (ccb.flags & ccb_flag::kPacked) != 0;
    if (!format_supported(bpp_code) || (!packed && (ccb.pre1 & pre1::kLrForm))) {
        format_log.warn("Cel engine: unimplemented source format (flags 0x{:08X}, PRE0 0x{:08X}, "
                        "PRE1 0x{:08X})",
                        ccb.flags, ccb.pre0, ccb.pre1);
        return {};
    }

    if (packed) {
        PackedRows rows(ccb, bpp_code);
        const u32 lsb = (ccb.cecontrol & cecontrol::kPdcLsbMask) >> cecontrol::kPdcLsbShift;
        return rasterise(ccb, rows, surface, lsb);
    }
    LiteralRows rows(ccb, bpp_code);
    return rasterise(ccb, rows, surface, (ccb.pre1 & pre1::kTlLsbMask) >> pre1::kTlLsbShift);
}

}  // namespace

u16 process_pixel(u32 ccb_flags, u32 pixc, u16 source, u16 frame, u16 amv) {
    // P-mode override from the CCB (POVER): 2 forces 0, 3 forces 1.
    switch ((ccb_flags & ccb_flag::kPoverMask) >> ccb_flag::kPoverShift) {
        case 2: source &= 0x7FFF; break;
        case 3: source |= 0x8000; break;
        default: break;
    }

    // The source pixel's P-mode bit picks which 16-bit half of PIXC applies.
    const u32 ppmp = (source & 0x8000) ? (pixc >> 16) : (pixc & 0xFFFF);
    const u32 dv2 = ppmp & 1;               // final divide by 2
    const u32 av = (ppmp >> 1) & 0x1F;      // constant / adder options
    const u32 s2 = (ppmp >> 6) & 3;         // secondary source
    const u32 dv1 = (ppmp >> 8) & 3;        // primary divider
    const u32 mf = (ppmp >> 10) & 7;        // primary multiplier - 1
    const u32 ms = (ppmp >> 13) & 3;        // multiplier source
    const bool s1_frame = (ppmp >> 15) & 1; // primary source: frame instead of cel

    const bool use_av = (ccb_flags & ccb_flag::kUseAv) != 0;
    const bool negate = use_av && (av & 0x01);   // subtract the secondary source
    const bool extend = use_av && (av & 0x02);   // sign-extend the secondary source
    const bool wrap = use_av && (av & 0x04);     // wrap instead of clamping
    const u32 dv3 = use_av ? (av >> 3) & 3 : 0;  // secondary divider (shift)
    const bool pxor = (ccb_flags & ccb_flag::kPxor) != 0;

    const u16 primary_in = s1_frame ? frame : source;
    u32 result[3];

    for (u32 ch = 0; ch < 3; ++ch) {
        s32 second = 0;
        switch (s2) {
            case 1: second = static_cast<s32>(av >> dv3); break;              // CCB constant
            case 2: second = static_cast<s32>(channel(frame, ch) >> dv3); break;  // frame
            case 3: second = static_cast<s32>(channel(source, ch) >> dv3); break; // cel
            default: break;                                                   // zero
        }

        u32 mul = mf;
        u32 div = dv1;
        switch (ms) {
            case 1: mul = (amv >> (6 - 3 * ch)) & 7; break;  // per-channel AMV
            case 2:                                           // from the pixel itself
                mul = channel(source, ch) >> 2;
                div = channel(source, ch) & 3;
                break;
            case 3: mul = channel(source, ch) >> 2; break;   // multiplier only
            default: break;                                   // CCB constant
        }
        const auto first =
            static_cast<s32>((channel(primary_in, ch) * (mul + 1)) >> divider_shift(div));

        const s32 a = pxor ? 0 : first;
        s32 b = negate ? ~second : (pxor ? (second ^ first) : second);
        if (extend) b = static_cast<s32>(static_cast<u32>(b) << 27) >> 27;  // 5-bit sign extend

        s32 sum = (a + b + (negate ? 1 : 0)) >> dv2;
        sum = wrap ? (sum & 0x1F) : std::clamp(sum, 0, 31);
        result[ch] = static_cast<u32>(sum);
    }

    auto out = static_cast<u16>((result[0] << 10) | (result[1] << 5) | result[2]);
    if (!(ccb_flags & ccb_flag::kNoBlk) && out == 0) out = kNearBlack;
    return out;
}

CelStats draw_cel(const Ccb& ccb, Framebuffer target) {
    LinearSurface surface(target);
    return draw(ccb, surface);
}

CelStats draw_cel(const Ccb& ccb, const VramTarget& target) {
    VramSurface surface(target);
    return draw(ccb, surface);
}

}  // namespace core::madam
