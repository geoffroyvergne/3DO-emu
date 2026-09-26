#include "core/madam/madam.hpp"

#include <vector>

#include "common/log.hpp"
#include "core/bus/bus.hpp"
#include "core/input/pbus.hpp"
#include "core/madam/cel.hpp"

namespace core::madam {

namespace {

// REGCTL0 modulo bits, each adding a pixel-width group (Opera's DecodeREGCTLMod).
constexpr u32 decode_modulo(u32 bits) {
    constexpr u32 kGroups[8] = {32, 512, 256, 1024, 64, 128, 256, 1024};
    u32 pixels = 0;
    for (u32 i = 0; i < 8; ++i)
        if (bits & (1u << i)) pixels += kGroups[i];
    return pixels * 4;  // bytes per line pair
}

constexpr u32 kAddressMask = 0x00FFFFFC;  // CCB pointers: 24-bit, word aligned
constexpr u32 kMaxCelsPerList = 16384;    // guards against corrupt, looping lists

}  // namespace

Madam::Madam(Bus& bus, const input::PlayerBus& player_bus, std::span<u8> ram)
    : bus_(bus), player_bus_(player_bus), ram_(ram) {
    reset();
}

void Madam::reset() {
    regs_.fill(0);
    reg(kRegMSysBits) = kMSysBits2MbDram1MbVram;
    reg(kRegPlayerLen) = kPlayerIdleLen;
    warned_offsets_.clear();
    x_pos_ = y_pos_ = 0;
    hdx_ = hdy_ = vdx_ = vdy_ = hddx_ = hddy_ = 0;
    pixc_ = 0;
    plut_ = {};
    cels_drawn_ = 0;
}

u32 Madam::mmio_read32(u32 offset) {
    if (offset >= kRegisterBytes) {
        warn_unmodelled("read", offset);
        return 0;
    }
    switch (offset) {
        case kRegRevision: return kRevisionGreen;
        case kRegStatBits: return 0;  // the list runs to completion on SPRSTRT
        case kRegXyPosH:
            return (static_cast<u32>(x_pos_) & 0xFFFF0000) | (static_cast<u32>(y_pos_) >> 16);
        case kRegXyPosL:
            return (static_cast<u32>(x_pos_) << 16) | (static_cast<u32>(y_pos_) & 0xFFFF);
        case kRegCeControl:
        case kRegRegCtl0:
        case kRegRegCtl1:
        case kRegRegCtl2:
        case kRegRegCtl3:
        case kRegVdlHead:
        case kRegCurrentCcb:
        case kRegNextCcb:
        case kRegPlutData:
        case kRegPData:
        case kRegMSysBits:
        case kRegMctl:
        case kRegPlayerDest:
        case kRegPlayerLen:
        case kRegPlayerOut: break;
        default: warn_unmodelled("read", offset); break;
    }
    return reg(offset);
}

void Madam::mmio_write32(u32 offset, u32 value) {
    if (offset >= kRegisterBytes) {
        warn_unmodelled("write", offset);
        return;
    }
    switch (offset) {
        case kRegRevision:
            // Debug output port: the OS writes its console text here one
            // character at a time (Opera prints it with KPRINT).
            if ((value & 0xFF) == '\n' || debug_line_.size() >= 200) {
                Log::info("[3DO] {}", debug_line_);
                debug_line_.clear();
            } else if ((value & 0xFF) >= 0x20 && (value & 0xFF) < 0x7F) {
                debug_line_ += static_cast<char>(value & 0xFF);
            }
            return;
        case kRegMSysBits: return;  // read-only
        default: break;
    }
    reg(offset) = value;

    switch (offset) {
        case kRegMctl:
            if (value & kMctlPlayXen) run_player_dma();
            break;
        case kRegSprStart: run_cel_list(); break;
        case kRegSprStop:
        case kRegSprContinue:
        case kRegSprPause: break;  // lists complete synchronously
        case kRegXyPosH:
            x_pos_ = static_cast<s32>((value & 0xFFFF0000) | (static_cast<u32>(x_pos_) & 0xFFFF));
            y_pos_ = static_cast<s32>((value << 16) | (static_cast<u32>(y_pos_) & 0xFFFF));
            break;
        case kRegXyPosL:
            x_pos_ = static_cast<s32>((static_cast<u32>(x_pos_) & 0xFFFF0000) | (value >> 16));
            y_pos_ = static_cast<s32>((static_cast<u32>(y_pos_) & 0xFFFF0000) | (value & 0xFFFF));
            break;
        case kRegCeControl:
        case kRegRegCtl0:
        case kRegRegCtl1:
        case kRegRegCtl2:
        case kRegRegCtl3:
        case kRegVdlHead:
        case kRegCurrentCcb:
        case kRegNextCcb:
        case kRegPlutData:
        case kRegPData:
        case kRegPlayerDest:
        case kRegPlayerLen:
        case kRegPlayerOut: break;
        default: warn_unmodelled("write", offset); break;
    }
}

void Madam::run_player_dma() {
    u32& dest = reg(kRegPlayerDest);
    u32& len = reg(kRegPlayerLen);
    u32& out = reg(kRegPlayerOut);

    if (static_cast<s32>(len) >= 0) {
        auto emit = [&](u32 word) {
            bus_.write32(dest, word);
            dest += 4;
            out += 4;
        };

        // The first word is control-port filler; LEN counts the bytes after it.
        emit(0xFFFFFFFF);

        const std::vector<u8> stream = player_bus_.serialize();  // word-aligned
        for (std::size_t i = 0; i + 3 < stream.size() && static_cast<s32>(len) > 0; i += 4) {
            emit((u32{stream[i]} << 24) | (u32{stream[i + 1]} << 16) | (u32{stream[i + 2]} << 8) |
                 u32{stream[i + 3]});
            len -= 4;
        }
        // Past the end of the chain the bus reads as all ones.
        while (static_cast<s32>(len) > 0) {
            emit(0xFFFFFFFF);
            len -= 4;
        }
        len = kPlayerIdleLen;
    }

    reg(kRegMctl) &= ~kMctlPlayXen;  // transfer done
    if (player_dma_done_) player_dma_done_();
}

u32 Madam::ram_word(u32 address) const {
    if (address + 4 > ram_.size()) return 0;
    return (u32{ram_[address]} << 24) | (u32{ram_[address + 1]} << 16) |
           (u32{ram_[address + 2]} << 8) | u32{ram_[address + 3]};
}

// Walks the CCB chain starting at NEXTCCB (Opera: opera_madam_cel_handle).
// Each CCB: flags, next, source, PLUT pointers (relative to the word after
// the pointer unless the matching *ABS flag is set), X, Y, then the optional
// groups selected by LDSIZE / LDPRS / LDPPMP / CCBPRE.
void Madam::run_cel_list() {
    if (ram_.empty()) return;
    const VramTarget target{
        ram_,
        reg(kRegRegCtl2),
        reg(kRegRegCtl3),
        decode_modulo(reg(kRegRegCtl0) & 0xFF),
        decode_modulo((reg(kRegRegCtl0) >> 8) & 0xFF),
        reg(kRegRegCtl1) & 0x3FF,
        (reg(kRegRegCtl1) >> 16) & 0x3FF,
    };

    u32 flags = 0;
    u32 count = 0;
    while ((reg(kRegNextCcb) & kAddressMask) != 0 && !(flags & ccb_flag::kLast)) {
        if (++count > kMaxCelsPerList) {
            Log::warn("MADAM: cel list longer than {} entries, stopping", kMaxCelsPerList);
            break;
        }
        u32 cur = reg(kRegNextCcb) & kAddressMask;
        if (cur + 4 * 16 > ram_.size()) break;

        flags = ram_word(cur);
        cur += 4;
        u32 next = ram_word(cur) & kAddressMask;
        if (!(flags & ccb_flag::kNpAbs)) next += cur + 4;
        cur += 4;
        u32 pdata = ram_word(cur) & kAddressMask;
        if (!(flags & ccb_flag::kSpAbs)) pdata += cur + 4;
        cur += 4;
        u32 plut_addr = ram_word(cur) & kAddressMask;
        if (!(flags & ccb_flag::kPpAbs)) plut_addr += cur + 4;
        cur += 4;
        reg(kRegNextCcb) = next;
        reg(kRegPData) = pdata;
        reg(kRegPlutData) = plut_addr;

        // Without YOXY the cel continues from the engine's current position.
        if (!(flags & ccb_flag::kSkip) && (flags & ccb_flag::kYoxy)) {
            x_pos_ = static_cast<s32>(ram_word(cur));
            y_pos_ = static_cast<s32>(ram_word(cur + 4));
        }
        cur += 8;

        if (flags & ccb_flag::kLdSize) {
            hdx_ = static_cast<s32>(ram_word(cur));
            hdy_ = static_cast<s32>(ram_word(cur + 4));
            vdx_ = static_cast<s32>(ram_word(cur + 8));
            vdy_ = static_cast<s32>(ram_word(cur + 12));
            cur += 16;
        }
        if (flags & ccb_flag::kLdPrs) {
            hddx_ = static_cast<s32>(ram_word(cur));
            hddy_ = static_cast<s32>(ram_word(cur + 4));
            cur += 8;
        }
        if (flags & ccb_flag::kLdPpmp) {
            pixc_ = ram_word(cur);
            cur += 4;
        }

        // Preamble: in the CCB (CCBPRE) or at the start of the source data.
        const bool packed = (flags & ccb_flag::kPacked) != 0;
        u32 pre0_word = 0, pre1_word = 0;
        if (flags & ccb_flag::kCcbPre) {
            pre0_word = ram_word(cur);
            cur += 4;
            if (!packed) {
                pre1_word = ram_word(cur);
                cur += 4;
            }
        } else {
            pre0_word = ram_word(pdata);
            pdata += 4;
            if (!packed) {
                pre1_word = ram_word(pdata);
                pdata += 4;
            }
        }
        reg(kRegCurrentCcb) = cur;

        if (flags & ccb_flag::kLdPlut) {
            u32 entries = 32;
            switch (pre0_word & pre0::kBppMask) {
                case 0:
                case pre0::kBpp1: entries = 2; break;
                case pre0::kBpp2: entries = 4; break;
                case pre0::kBpp4: entries = 16; break;
                default: break;
            }
            for (u32 i = 0; i < entries; ++i) {
                const u32 a = plut_addr + 2 * i;
                plut_[i] = a + 1 < ram_.size() ? static_cast<u16>((ram_[a] << 8) | ram_[a + 1]) : 0;
            }
        }
        if (flags & ccb_flag::kSkip) continue;

        Ccb ccb;
        ccb.flags = flags;
        ccb.x_pos = x_pos_;
        ccb.y_pos = y_pos_;
        ccb.hdx = hdx_;
        ccb.hdy = hdy_;
        ccb.vdx = vdx_;
        ccb.vdy = vdy_;
        ccb.hddx = hddx_;
        ccb.hddy = hddy_;
        ccb.pixc = pixc_;
        ccb.pre0 = pre0_word;
        ccb.pre1 = pre1_word;
        ccb.source = pdata < ram_.size() ? std::span<const u8>(ram_).subspan(pdata) : std::span<const u8>{};
        ccb.plut = plut_;
        ccb.cecontrol = reg(kRegCeControl);

        const CelStats stats = draw_cel(ccb, target);
        ++cels_drawn_;
        // The engine leaves its position just below the cel it drew.
        x_pos_ = static_cast<s32>(static_cast<u32>(x_pos_) + static_cast<u32>(vdx_) * stats.rows);
        y_pos_ = static_cast<s32>(static_cast<u32>(y_pos_) + static_cast<u32>(vdy_) * stats.rows);
    }
}

void Madam::warn_unmodelled(const char* access, u32 offset) {
    if (warned_offsets_.insert(offset).second)
        Log::warn("MADAM: unmodelled register {} at +0x{:03X}", access, offset);
}

}  // namespace core::madam
