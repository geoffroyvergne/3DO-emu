#include "core/dsp/dspp.hpp"

#include "core/dsp/audio_dma.hpp"

namespace core::dsp {

namespace {

constexpr u32 kSystemTicks = 568;          // ceil(25 MHz / 44.1 kHz + 1), Opera
constexpr u16 kSleep = 0x8380;             // what code memory holds at power-on
constexpr u32 kMaxInstructionsPerSample = 16384;  // guard against programs that never sleep
constexpr u32 kTopBit = 0x80000000;

constexpr u32 bits(u32 v, u32 shift, u32 width) { return (v >> shift) & ((1u << width) - 1); }

constexpr bool add_carry(u32 a, u32 b, u32 y) {
    return (a & b & kTopBit) || (a & ~y & kTopBit) || (b & ~y & kTopBit);
}
constexpr bool sub_carry(u32 a, u32 b, u32 y) {
    return (a & ~b & kTopBit) || (a & ~y & kTopBit) || (~b & ~y & kTopBit);
}
constexpr bool add_overflow(u32 a, u32 b, u32 y) {
    return (a & b & ~y & kTopBit) || (~a & ~b & y & kTopBit);
}
constexpr bool sub_overflow(u32 a, u32 b, u32 y) {
    return (a & ~b & ~y & kTopBit) || (~a & b & y & kTopBit);
}

// 13-bit signed immediate, optionally shifted left by 3 ("justify").
constexpr u16 immediate(u16 word) {
    const s32 imm = static_cast<s32>(static_cast<u32>(word & 0x1FFF) << 19) >> 19;
    const u32 shift = bits(word, 13, 1) ? 3 : 0;
    return static_cast<u16>(static_cast<u32>(imm) << shift);
}

}  // namespace

void Dspp::build_tables() {
    // Register number -> I-memory address, for each of the 8 register maps.
    for (u32 map = 0; map < 8; ++map) {
        for (u32 reg = 0; reg < 16; ++reg) {
            const u32 x = (reg >> 2) & 1, y = (reg >> 3) & 1;
            u32 twi = x;
            switch (map) {
                case 4: twi = y; break;
                case 5: twi = !y; break;
                case 6: twi = x & y; break;
                case 7: twi = x | y; break;
                default: break;
            }
            reg_conv_[map][reg] = static_cast<u16>((reg & 7) | (twi << 8) | ((reg >> 3) << 9));
        }
    }

    // ALU instructions: which operands they request and the shifter mode.
    for (u32 i = 0; i < 0x8000; ++i) {
        u8 req = 0;
        if (bits(i, 0, 4) == 0x8) req |= kReqBs;
        const bool m2sel = bits(i, 12, 1) != 0;
        auto mux = [&](u32 sel) {
            switch (sel) {
                case 1: req |= kReqAlu1; break;
                case 2: req |= kReqAlu2; break;
                case 3: req |= kReqMult1; if (m2sel) req |= kReqMult2; break;
                default: break;
            }
        };
        mux(bits(i, 10, 2));  // MUXA
        mux(bits(i, 8, 2));   // MUXB
        const u32 shift = bits(i, 0, 4) | ((bits(i, 4, 4) & 8) << 1);  // ALU bit 3 = logical
        alu_decode_[i] = {req, static_cast<u8>(shift)};
    }

    // Conditional branches: [condition bits 14:10][Z N C V exact].
    for (u32 b = 0; b < 32; ++b) {
        const bool flagm0 = b & 1, flagm1 = (b >> 1) & 1, flagsel = (b >> 2) & 1;
        const bool mode0 = (b >> 3) & 1, mode1 = (b >> 4) & 1;
        for (u32 f = 0; f < 32; ++f) {
            const bool exact = f & 1, overflow = (f >> 1) & 1, carry = (f >> 2) & 1;
            const bool negative = (f >> 3) & 1, zero = (f >> 4) & 1;

            const bool md1 = !mode1 && mode0, md2 = mode1 && !mode0, md3 = mode1 && mode0;
            const bool stat0 = flagsel ? carry : negative;
            const bool stat1 = flagsel ? zero : overflow;
            const bool nstat0 = stat0 != md2, nstat1 = stat1 != md2;
            const bool tdcare1 = !flagm1 || nstat0;
            const bool tdcare0 = !flagm0 || nstat1;
            const bool rdcare = !flagm1 && !flagm0;
            const bool md12s = tdcare1 && tdcare0 && (mode1 != mode0) && !rdcare;
            const bool super0 = md1 && !flagsel && rdcare;
            const bool super1 = md1 && flagsel && rdcare;
            const bool sds = (super0 && zero && exact) || (super1 && !(zero && exact));
            const bool nv = (((negative != overflow) || (zero && flagm0)) != flagm1) && !flagsel;
            const bool cz = ((carry && !zero) != flagm0) && flagsel && !flagm1;
            const bool xact = (exact != flagm0) && flagsel && flagm1;
            const bool md3s = (nv || cz || xact) && md3;
            branch_table_[b][f] = md12s || md3s || sds;
        }
    }
}

void Dspp::init() {
    nmem_.fill(kSleep);
    imem_.fill(0);
    cpu_supply_.fill(false);
    audio_out_status_ = 0;
    sema4_status_ = 0;
    sema4_data_ = 0;
    int_value_ = 0;
    dspp_rld_ = kSystemTicks;
    running_ = false;
    gen_fiq_ = false;
    reset();
}

void Dspp::reset() {
    dspp_cnt_ = dspp_rld_;
    pc_ = 0;
    rbase4_ = 0;
    reg_map_ = 0;
    op_mask_ = 0xFF;
}

u16 Dspp::noise() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<u16>(rng_);
}

u16 Dspp::register_address(u32 reg) const {
    return static_cast<u16>(reg_conv_[reg_map_][reg & 0xF] ^ rbase4_);
}

u16 Dspp::read(u32 address) {
    switch (address) {
        case 0xEA: return noise();
        case 0xEB: return audio_out_status_;
        case 0xEC: return sema4_status_;
        case 0xED: return sema4_data_;
        case 0xEE: return pc_;
        case 0xEF: return static_cast<u16>(dspp_cnt_);
        default: break;
    }
    if (address >= 0xF0 && address <= 0xFC) {  // input FIFO data (consuming)
        const u32 ch = address - 0xF0;
        ++counters_.data[ch];
        if (cpu_supply_[ch]) {
            cpu_supply_[ch] = false;
            return imem_[address - 0x80];
        }
        return fifos_.pop_input(address & 0xF);
    }
    if (address >= 0x70 && address <= 0x7C) {  // input FIFO data (peek)
        const u32 ch = address - 0x70;
        if (cpu_supply_[ch]) {
            cpu_supply_[ch] = false;
            return imem_[address];
        }
        return fifos_.peek_input(address & 0xF);
    }
    if (address >= 0xD0 && address <= 0xDE)  // input FIFO status
        return ++counters_.status[address & 0xF], cpu_supply_[address & 0xF] ? 2 : fifos_.input_status(address & 0xF);
    if (address >= 0xE0 && address <= 0xE3) return fifos_.output_status(address & 0xF);

    const u32 a = address - 0x100;
    if (a < 0x200) return imem_[a | 0x100];
    return imem_[a & 0x7F];
}

void Dspp::write(u32 address, u16 value) {
    address &= 0x3FF;
    switch (address) {
        case 0x3EB: audio_out_status_ = value; return;
        case 0x3EC: sema4_status_ |= 0x01; return;  // DSP acknowledges
        case 0x3ED:
            sema4_data_ = value;
            sema4_status_ = 0x4;  // DSP wrote the semaphore last
            return;
        case 0x3EE:
            int_value_ = value;
            gen_fiq_ = true;
            return;
        case 0x3EF: dspp_rld_ = static_cast<s16>(value); return;
        case 0x3F0: case 0x3F1: case 0x3F2: case 0x3F3:
            fifos_.push_output(address & 0xF, value);
            return;
        case 0x3FD: return;  // flush output FIFO
        case 0x3FE: case 0x3FF: imem_[address] = value; return;  // DAC left / right
        default: break;
    }
    if (address < 0x100) return;
    const u32 a = address - 0x100;
    if (a < 0x200)
        imem_[a | 0x100] = value;
    else
        imem_[a + 0x100] = value;
}

// Single operand (MOVE / MOVEREG source).
u16 Dspp::load_operand1() {
    const u16 op = nmem_[pc_++ & 0x7FF];
    switch (bits(op, 13, 3)) {
        case 0: case 1: case 2: case 3: {  // three-register format: R3 is the operand
            const u16 v = read(register_address(bits(op, 10, 4)));
            return bits(op, 14, 1) ? read(v) : v;
        }
        case 4: {  // address
            const u16 v = read(bits(op, 0, 10));
            return bits(op, 10, 1) ? read(v) : v;
        }
        case 5: {  // one/two-register format: R1
            const u16 v = read(register_address(bits(op, 0, 4)));
            return bits(op, 4, 1) ? read(v) : v;
        }
        default: return immediate(op);
    }
}

// Operands for an ALU instruction (Opera: dsp_operand_load).
void Dspp::load_operands(u32 count) {
    writeback_ = 0;
    if (count == 0) {
        if (requests_ == 0) return;
        count = 4;
    }
    std::array<u16, 8> ops{};
    u32 n = 0;
    u16 explicit_writeback = 0;

    do {
        const u16 op = nmem_[pc_++ & 0x7FF];
        switch (bits(op, 13, 3)) {
            case 0: case 1: case 2: case 3: {  // three registers; only R1 can be written back
                u16 v = read(register_address(bits(op, 10, 4)));
                ops[n++] = bits(op, 14, 1) ? read(v) : v;
                v = read(register_address(bits(op, 5, 4)));
                ops[n++] = bits(op, 9, 1) ? read(v) : v;
                writeback_ = register_address(bits(op, 0, 4));
                v = read(writeback_);
                ops[n++] = bits(op, 4, 1) ? read(v) : v;
                break;
            }
            case 4: {  // address
                writeback_ = static_cast<u16>(bits(op, 0, 10));
                const u16 v = read(writeback_);
                ops[n++] = bits(op, 10, 1) ? read(v) : v;
                if (bits(op, 11, 1)) explicit_writeback = writeback_;
                break;
            }
            case 5: {  // one or two registers
                if (bits(op, 10, 1)) {
                    writeback_ = register_address(bits(op, 5, 4));
                    if (bits(op, 9, 1)) writeback_ = read(writeback_);
                    ops[n++] = read(writeback_);
                    if (bits(op, 12, 1)) explicit_writeback = writeback_;
                }
                writeback_ = register_address(bits(op, 0, 4));
                if (bits(op, 4, 1)) writeback_ = read(writeback_);
                ops[n++] = read(writeback_);
                if (bits(op, 11, 1)) explicit_writeback = writeback_;
                break;
            }
            default:  // immediate
                ops[n] = immediate(op);
                writeback_ = ops[n++];
                break;
        }
    } while (n < count && n + 3 <= ops.size());

    requests_ &= op_mask_;
    u32 used = 0;
    if (requests_ & kReqMult1) mult1_ = static_cast<s16>(ops[used++]);
    if (requests_ & kReqMult2) mult2_ = static_cast<s16>(ops[used++]);
    if (requests_ & kReqAlu1) alu1_ = static_cast<s16>(ops[used++]);
    if (requests_ & kReqAlu2) alu2_ = static_cast<s16>(ops[used++]);
    if (requests_ & kReqBs) bs_ = static_cast<s16>(ops[used++]);

    // A spare operand is the result's destination (Opera's rule).
    if (n != used) {
        if (explicit_writeback) writeback_ = explicit_writeback;
    } else {
        writeback_ = explicit_writeback;
    }
}

void Dspp::write_input(u32 address, u16 value) {
    if (address >= 0x70 && address <= 0x7C) {
        cpu_supply_[address - 0x70] = true;
        imem_[address & 0x7F] = value;
    } else if (!(address & 0x80)) {
        imem_[address & 0x7F] = value;
    }
}

u16 Dspp::read_output(u32 address) const {
    switch (address) {
        case 0x3EB: return audio_out_status_;
        case 0x3EC: return sema4_status_;
        case 0x3ED: return sema4_data_;
        case 0x3EE: return int_value_;
        case 0x3EF: return static_cast<u16>(dspp_rld_);
        default: return imem_[address & 0x3FF];
    }
}

void Dspp::write_semaphore(u32 value) {
    sema4_data_ = static_cast<u16>(value);
    sema4_status_ = 0x8;  // ARM wrote the semaphore last
}

u32 Dspp::run_sample() {
    if (running_) {
        reset();
        u32 y = 0, aop = 0, bop = 0;
        u16 return_address = 0;
        bool zero = false, negative = false, carry = false, overflow = false, exact = false;

        for (u32 steps = 0; steps < kMaxInstructionsPerSample; ++steps) {
            const u16 inst = nmem_[pc_++ & 0x7FF];

            if (inst & 0x8000) {  // control instruction
                const u32 op = bits(inst, 7, 8);
                const u16 target = static_cast<u16>(bits(inst, 0, 10));
                bool sleep = false;
                if (op == 0) {
                    // NOP
                } else if (op == 1) {
                    pc_ = static_cast<u16>((y >> 16) & 0x3FF);  // branch to accumulator
                } else if (op == 2) {
                    rbase4_ = (bits(inst, 0, 10) & 0x3F) << 2;
                } else if (op == 3) {
                    reg_map_ = bits(inst, 0, 10) & 7;
                } else if (op == 4) {
                    pc_ = return_address;  // RTS
                } else if (op == 5) {
                    op_mask_ = static_cast<u8>(~(bits(inst, 0, 10) & 0x1F));
                } else if (op == 6) {
                    // unused
                } else if (op == 7) {
                    sleep = true;
                } else if (op < 16) {
                    pc_ = target;  // jump
                } else if (op < 24) {
                    return_address = pc_;  // jsr
                    pc_ = target;
                } else if (op < 32) {
                    pc_ = target;  // "branch only if was branched"
                } else if (op < 48) {  // MOVEREG: to register R1 (optionally indirect)
                    const u16 value = load_operand1();
                    u16 address = register_address(bits(inst, 0, 4));
                    if (bits(inst, 4, 1)) address = read(address);
                    write(address, value);
                } else if (op < 64) {  // MOVE: to address
                    const u16 value = load_operand1();
                    u16 address = target;
                    if (bits(inst, 10, 1)) address = read(address);
                    write(address, value);
                } else {  // conditional branch
                    const u32 flags = (u32{zero} << 4) | (u32{negative} << 3) | (u32{carry} << 2) |
                                      (u32{overflow} << 1) | u32{exact};
                    if (branch_table_[bits(inst, 10, 5)][flags]) pc_ = target;
                }
                if (sleep) break;
                continue;
            }

            // ALU instruction.
            const DecodedAlu decoded = alu_decode_[inst];
            requests_ = decoded.requests;
            bs_ = decoded.shift;
            load_operands(bits(inst, 13, 2));

            const u32 alu = bits(inst, 4, 4);
            const bool m2sel = bits(inst, 12, 1) != 0;
            const bool carry_select = alu == 3 || alu == 5;  // ACSBU: add/sub with carry
            const auto mult_y = [&](bool mask_first) {
                const s64 yy = static_cast<s32>(y) >> 15;
                if (mask_first) return static_cast<u32>(static_cast<s64>(mult1_) * (yy & ~1LL));
                return static_cast<u32>(static_cast<s64>(mult1_) * yy) & ~1u;
            };
            const auto mult_mult = [&] {
                return static_cast<u32>(static_cast<s64>(mult1_) * mult2_ * 2);
            };
            const auto alu_in = [](s16 v) { return static_cast<u32>(static_cast<s32>(v) << 16); };

            switch (bits(inst, 10, 2)) {  // MUXA
                case 3:
                    if (!m2sel)
                        aop = carry_select ? (carry ? alu_in(mult1_) : 0u) : mult_y(true);
                    else
                        aop = mult_mult();
                    break;
                case 1: aop = alu_in(alu1_); break;
                case 2: aop = alu_in(alu2_); break;
                default: aop = y; break;
            }
            if (carry_select) {
                bop = carry ? 0x10000u : 0u;
            } else {
                switch (bits(inst, 8, 2)) {  // MUXB
                    case 0: bop = y; break;
                    case 1: bop = alu_in(alu1_); break;
                    case 2: bop = alu_in(alu2_); break;
                    default: bop = m2sel ? mult_mult() : mult_y(false); break;
                }
            }

            carry = overflow = false;
            switch (alu) {
                case 0: y = aop; break;
                case 1: y = 0 - bop; carry = sub_carry(0, bop, y); overflow = sub_overflow(0, bop, y); break;
                case 2: case 3: y = aop + bop; carry = add_carry(aop, bop, y); overflow = add_overflow(aop, bop, y); break;
                case 4: case 5: y = aop - bop; carry = sub_carry(aop, bop, y); overflow = sub_overflow(aop, bop, y); break;
                case 6: y = aop + 0x1000; carry = add_carry(aop, 0x1000, y); overflow = add_overflow(aop, 0x1000, y); break;
                case 7: y = aop - 0x1000; carry = sub_carry(aop, 0x1000, y); overflow = sub_overflow(aop, 0x1000, y); break;
                case 8: y = aop; break;
                case 9: y = ~aop; break;
                case 10: y = aop & bop; break;
                case 11: y = ~(aop & bop); break;
                case 12: y = aop | bop; break;
                case 13: y = ~(aop | bop); break;
                case 14: y = aop ^ bop; break;
                default: y = ~(aop ^ bop); break;
            }
            zero = (y & 0xFFFF0000) == 0;
            negative = (y >> 31) != 0;
            exact = (y & 0x0000F000) == 0;

            // Barrel shifter.
            const auto asr = [&](int s) { y = static_cast<u32>(static_cast<s32>(y) >> s); };
            switch (bs_) {
                case 1: case 17: y <<= 1; break;
                case 2: case 18: y <<= 2; break;
                case 3: case 19: y <<= 3; break;
                case 4: case 20: y <<= 4; break;
                case 5: case 21: y <<= 5; break;
                case 6: case 22: y <<= 8; break;
                case 9: asr(16); break;
                case 10: asr(8); break;
                case 11: asr(5); break;
                case 12: asr(4); break;
                case 13: asr(3); break;
                case 14: asr(2); break;
                case 15: asr(1); break;
                case 7: case 23:  // clip on overflow
                    if (overflow) y = negative ? 0x7FFFF000u : 0x80000000u;
                    break;
                case 8: case 24:  // rotate left through carry
                    carry = static_cast<s32>(y) < 0;
                    y = ((y << 1) & 0xFFFE0000u) | (carry ? 0x10000u : 0u) | (y & 0xF000u);
                    break;
                case 25: y >>= 16; break;
                case 26: y >>= 8; break;
                case 27: y >>= 5; break;
                case 28: y >>= 4; break;
                case 29: y >>= 3; break;
                case 30: y >>= 2; break;
                case 31: y >>= 1; break;
                default: break;
            }
            if (writeback_) write(writeback_, static_cast<u16>(static_cast<s32>(y) >> 16));
        }

        dspp_cnt_ -= static_cast<s32>(kSystemTicks);
        if (dspp_cnt_ <= 0) dspp_cnt_ += dspp_rld_;
    }
    return (u32{imem_[0x3FF]} << 16) | imem_[0x3FE];
}

}  // namespace core::dsp
