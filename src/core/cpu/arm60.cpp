#include "core/cpu/arm60.hpp"

#include <algorithm>
#include <bit>

#include "common/log.hpp"
#include "core/bus/bus.hpp"

namespace core::cpu {

namespace {

enum DpOpcode : u32 {
    kAnd = 0x0, kEor = 0x1, kSub = 0x2, kRsb = 0x3,
    kAdd = 0x4, kAdc = 0x5, kSbc = 0x6, kRsc = 0x7,
    kTst = 0x8, kTeq = 0x9, kCmp = 0xA, kCmn = 0xB,
    kOrr = 0xC, kMov = 0xD, kBic = 0xE, kMvn = 0xF,
};

enum ShiftType : u32 { kLsl = 0, kLsr = 1, kAsr = 2, kRor = 3 };

struct Shifted {
    u32 value;
    bool carry;
};

// Cycle costs, as Opera models the ARM60 on the 3DO bus: sequential and
// internal cycles cost 1, non-sequential memory accesses cost 2.
constexpr u32 kS = 1;
constexpr u32 kN = 2;
constexpr u32 kI = 1;

constexpr bool bit(u32 value, u32 n) {
    return ((value >> n) & 1u) != 0;
}

// Shift amount encoded as a 5-bit immediate. Amount 0 has special meanings
// for LSR/ASR (#32) and ROR (RRX).
constexpr Shifted shift_by_immediate(u32 type, u32 rm, u32 amount, bool carry_in) {
    switch (type) {
        case kLsl:
            if (amount == 0) return {rm, carry_in};
            return {rm << amount, bit(rm, 32 - amount)};
        case kLsr:
            if (amount == 0) return {0, bit(rm, 31)};
            return {rm >> amount, bit(rm, amount - 1)};
        case kAsr:
            if (amount == 0) return {static_cast<u32>(static_cast<s32>(rm) >> 31), bit(rm, 31)};
            return {static_cast<u32>(static_cast<s32>(rm) >> amount), bit(rm, amount - 1)};
        default:  // kRor
            if (amount == 0) return {(carry_in ? 1u << 31 : 0u) | (rm >> 1), bit(rm, 0)};
            return {std::rotr(rm, static_cast<int>(amount)), bit(rm, amount - 1)};
    }
}

// Shift amount taken from the bottom byte of Rs. Amount 0 leaves Rm and C untouched.
constexpr Shifted shift_by_register(u32 type, u32 rm, u32 amount, bool carry_in) {
    if (amount == 0) return {rm, carry_in};
    switch (type) {
        case kLsl:
            if (amount < 32) return {rm << amount, bit(rm, 32 - amount)};
            if (amount == 32) return {0, bit(rm, 0)};
            return {0, false};
        case kLsr:
            if (amount < 32) return {rm >> amount, bit(rm, amount - 1)};
            if (amount == 32) return {0, bit(rm, 31)};
            return {0, false};
        case kAsr:
            if (amount < 32)
                return {static_cast<u32>(static_cast<s32>(rm) >> amount), bit(rm, amount - 1)};
            return {static_cast<u32>(static_cast<s32>(rm) >> 31), bit(rm, 31)};
        default: {  // kRor
            const u32 rot = amount & 31;
            if (rot == 0) return {rm, bit(rm, 31)};
            return {std::rotr(rm, static_cast<int>(rot)), bit(rm, rot - 1)};
        }
    }
}

// a + b + carry_in, with the ARM carry-out and signed-overflow flags.
struct AddResult {
    u32 value;
    bool carry;
    bool overflow;
};

constexpr AddResult add_with_carry(u32 a, u32 b, bool carry_in) {
    const u64 wide = u64{a} + u64{b} + (carry_in ? 1u : 0u);
    const auto value = static_cast<u32>(wide);
    return {value, (wide >> 32) != 0, bit(~(a ^ b) & (a ^ value), 31)};
}

Bank bank_of(u32 mode_bits) {
    switch (mode_bits) {
        case 0x10: return kBankUser;
        case 0x11: return kBankFiq;
        case 0x12: return kBankIrq;
        case 0x13: return kBankSvc;
        case 0x17: return kBankAbort;
        case 0x1B: return kBankUndef;
        default:
            // 26-bit modes (0x00-0x03) exist on the ARM60 but the 3DO runs in
            // 32-bit mode (its ROM sits above 64 MB); treat as unverified.
            Log::warn("ARM60: unsupported CPU mode 0x{:02X}, using User bank", mode_bits);
            return kBankUser;
    }
}

u32 load_word_rotated(Bus& bus, u32 address) {
    // Unaligned LDR returns the aligned word rotated so the addressed byte
    // lands in the low byte (Opera: ROTR(word, (addr & 3) * 8)).
    const u32 word = bus.read32(address & ~3u);
    return std::rotr(word, static_cast<int>((address & 3) * 8));
}

}  // namespace

const std::array<Arm60::Handler, Arm60::kDecodeTableSize> Arm60::kDecodeTable =
    Arm60::build_decode_table();

std::array<Arm60::Handler, Arm60::kDecodeTableSize> Arm60::build_decode_table() {
    std::array<Handler, kDecodeTableSize> table{};
    for (std::size_t i = 0; i < kDecodeTableSize; ++i) {
        const auto hi = static_cast<u32>(i >> 4);   // instr[27:20]
        const auto lo = static_cast<u32>(i & 0xF);  // instr[7:4]
        Handler handler = &Arm60::op_undefined;

        switch (hi >> 5) {  // instr[27:25]
            case 0b000:
            case 0b001: {
                const bool immediate = bit(hi, 5);
                const bool set_flags = bit(hi, 0);
                const u32 opcode = (hi >> 1) & 0xF;

                if (!immediate && lo == 0b1001) {
                    if ((hi & 0xFC) == 0x00) handler = &Arm60::op_multiply;          // MUL/MLA
                    else if ((hi & 0xFB) == 0x10) handler = &Arm60::op_swap;         // SWP/SWPB
                    // everything else here (long multiply, ...) is not ARMv3
                } else if (!immediate && (lo & 0b1001) == 0b1001) {
                    // halfword transfers: ARMv4, undefined here
                } else if (!set_flags && opcode >= kTst && opcode <= kCmn) {
                    // PSR transfers live in the compare opcodes with S clear.
                    if (!immediate && (hi & 0xFB) == 0x10 && lo == 0) handler = &Arm60::op_mrs;
                    else if (!immediate && (hi & 0xFB) == 0x12 && lo == 0) handler = &Arm60::op_msr;
                    else if (immediate && (hi & 0xFB) == 0x32) handler = &Arm60::op_msr;
                } else {
                    handler = &Arm60::op_data_processing;
                }
                break;
            }
            case 0b010: handler = &Arm60::op_single_transfer; break;
            case 0b011:
                // Register offset with instr[4] set is the undefined-instruction space.
                if (!bit(lo, 0)) handler = &Arm60::op_single_transfer;
                break;
            case 0b100: handler = &Arm60::op_block_transfer; break;
            case 0b101: handler = &Arm60::op_branch; break;
            case 0b110:
            case 0b111:
                // Coprocessor space: the 3DO has no coprocessor, so these trap.
                if ((hi & 0xF0) == 0xF0) handler = &Arm60::op_swi;
                break;
        }
        table[i] = handler;
    }
    return table;
}

Arm60::Arm60(Bus& bus) : bus_(bus) {
    reset();
}

void Arm60::reset() {
    regs_ = {};
    regs_.cpsr = static_cast<u32>(Mode::Supervisor) | psr::kIrqDisable | psr::kFiqDisable;
    set_pc(vector::kReset);
    pipeline_flushed_ = false;
    irq_line_ = false;
    fiq_line_ = false;
}

u32 Arm60::execute_cycles(u32 cycles) {
    u32 executed = 0;
    while (executed < cycles) executed += step();
    return executed;
}

u32 Arm60::step() {
    // Interrupts are sampled between instructions; FIQ has priority.
    // The return address is the next instruction + 4, as for real hardware
    // (handlers return with SUBS pc, lr, #4).
    if (fiq_line_ && !(regs_.cpsr & psr::kFiqDisable)) {
        enter_exception(Mode::Fiq, vector::kFiq, pc() + 4);
        return 2 * kS + kN;
    }
    if (irq_line_ && !(regs_.cpsr & psr::kIrqDisable)) {
        enter_exception(Mode::Irq, vector::kIrq, pc() + 4);
        return 2 * kS + kN;
    }

    if (watch_pc_ && !watch_hit_ && pc() == *watch_pc_) watch_hit_ = regs_;

    const u32 instr = bus_.fetch32(pc());
    pipeline_flushed_ = false;

    // TODO: 3DO DRAM/ROM wait states are not modelled; every S-cycle costs 1.
    u32 cycles = 1;
    if (condition_passed(instr >> 28)) cycles = (this->*kDecodeTable[decode_index(instr)])(instr);

    if (!pipeline_flushed_) regs_.r[15] += 4;
    return cycles;
}

void Arm60::set_cpsr(u32 value) {
    const Bank old_bank = bank_of(regs_.cpsr & psr::kModeMask);
    const Bank new_bank = bank_of(value & psr::kModeMask);

    if (old_bank != new_bank) {
        auto& r = regs_.r;
        regs_.r13_r14[old_bank] = {r[13], r[14]};
        auto& saved_high = (old_bank == kBankFiq) ? regs_.r8_r12_fiq : regs_.r8_r12_user;
        for (std::size_t i = 0; i < 5; ++i) saved_high[i] = r[8 + i];

        r[13] = regs_.r13_r14[new_bank][0];
        r[14] = regs_.r13_r14[new_bank][1];
        const auto& loaded_high = (new_bank == kBankFiq) ? regs_.r8_r12_fiq : regs_.r8_r12_user;
        for (std::size_t i = 0; i < 5; ++i) r[8 + i] = loaded_high[i];
    }
    regs_.cpsr = value;
}

Bank Arm60::current_bank() const {
    return bank_of(regs_.cpsr & psr::kModeMask);
}

u32 Arm60::user_reg(u32 index) const {
    const Bank bank = current_bank();
    if (index >= 8 && index <= 12 && bank == kBankFiq) return regs_.r8_r12_user[index - 8];
    if (index >= 13 && index <= 14 && bank != kBankUser) return regs_.r13_r14[kBankUser][index - 13];
    return regs_.r[index];
}

void Arm60::set_user_reg(u32 index, u32 value) {
    const Bank bank = current_bank();
    if (index >= 8 && index <= 12 && bank == kBankFiq)
        regs_.r8_r12_user[index - 8] = value;
    else if (index >= 13 && index <= 14 && bank != kBankUser)
        regs_.r13_r14[kBankUser][index - 13] = value;
    else if (index == 15)
        write_pc(value);
    else
        regs_.r[index] = value;
}

bool Arm60::condition_passed(u32 cond) const {
    const u32 f = regs_.cpsr;
    const bool n = (f & psr::kN) != 0;
    const bool z = (f & psr::kZ) != 0;
    const bool c = (f & psr::kC) != 0;
    const bool v = (f & psr::kV) != 0;

    switch (cond) {
        case 0x0: return z;               // EQ
        case 0x1: return !z;              // NE
        case 0x2: return c;               // CS/HS
        case 0x3: return !c;              // CC/LO
        case 0x4: return n;               // MI
        case 0x5: return !n;              // PL
        case 0x6: return v;               // VS
        case 0x7: return !v;              // VC
        case 0x8: return c && !z;         // HI
        case 0x9: return !c || z;         // LS
        case 0xA: return n == v;          // GE
        case 0xB: return n != v;          // LT
        case 0xC: return !z && n == v;    // GT
        case 0xD: return z || n != v;     // LE
        case 0xE: return true;            // AL
        default: return false;            // NV (never, on ARMv3)
    }
}

void Arm60::set_nzcv(u32 result, bool carry, bool overflow) {
    u32 f = regs_.cpsr & ~(psr::kN | psr::kZ | psr::kC | psr::kV);
    if (bit(result, 31)) f |= psr::kN;
    if (result == 0) f |= psr::kZ;
    if (carry) f |= psr::kC;
    if (overflow) f |= psr::kV;
    regs_.cpsr = f;
}

void Arm60::set_nzc(u32 result, bool carry) {
    set_nzcv(result, carry, (regs_.cpsr & psr::kV) != 0);
}

void Arm60::set_nz(u32 result) {
    set_nzcv(result, carry_flag(), (regs_.cpsr & psr::kV) != 0);
}

Arm60::ShifterResult Arm60::operand2_immediate(u32 instr) const {
    const u32 imm8 = instr & 0xFF;
    const u32 rotate = ((instr >> 8) & 0xF) * 2;
    const u32 value = std::rotr(imm8, static_cast<int>(rotate));
    return {value, rotate == 0 ? carry_flag() : bit(value, 31)};
}

Arm60::ShifterResult Arm60::operand2_register(u32 instr) const {
    const u32 rm_index = instr & 0xF;
    const u32 type = (instr >> 5) & 0x3;

    if (!bit(instr, 4)) {
        const u32 amount = (instr >> 7) & 0x1F;
        const auto [value, carry] = shift_by_immediate(type, regs_.r[rm_index], amount, carry_flag());
        return {value, carry};
    }

    // Register-specified shift takes an extra internal cycle, so the PC reads 12 ahead.
    const u32 rm = regs_.r[rm_index] + (rm_index == 15 ? 4u : 0u);
    const u32 amount = regs_.r[(instr >> 8) & 0xF] & 0xFF;
    const auto [value, carry] = shift_by_register(type, rm, amount, carry_flag());
    return {value, carry};
}

void Arm60::write_pc(u32 address) {
    if (trace_enabled_) {
        const u32 to = address & ~3u;
        // Skip tight backward loops so they do not flush the history.
        if (!(to < pc() && pc() - to < 0x40)) trace_[trace_count_++ % kTraceSize] = {pc(), to};
    }
    set_pc(address);
    pipeline_flushed_ = true;
}

std::vector<Arm60::Transfer> Arm60::recent_transfers() const {
    std::vector<Transfer> out;
    const std::size_t n = std::min(trace_count_, kTraceSize);
    for (std::size_t i = trace_count_ - n; i < trace_count_; ++i) out.push_back(trace_[i % kTraceSize]);
    return out;
}

void Arm60::restore_cpsr_from_spsr() {
    const Bank bank = current_bank();
    if (bank == kBankUser) {
        Log::warn("ARM60: SPSR restore in User mode at 0x{:08X} (unpredictable)", pc());
        return;
    }
    set_cpsr(regs_.spsr[bank]);
}

void Arm60::enter_exception(Mode mode, u32 vector_address, u32 return_address) {
    const u32 old_cpsr = regs_.cpsr;
    u32 new_cpsr = (old_cpsr & ~psr::kModeMask) | static_cast<u32>(mode) | psr::kIrqDisable;
    if (mode == Mode::Fiq) new_cpsr |= psr::kFiqDisable;

    set_cpsr(new_cpsr);
    regs_.spsr[current_bank()] = old_cpsr;
    regs_.r[14] = return_address;
    write_pc(vector_address);
}

// --- Data processing ---------------------------------------------------------

u32 Arm60::op_data_processing(u32 instr) {
    const bool immediate = bit(instr, 25);
    const bool set_flags = bit(instr, 20);
    const bool register_shift = !immediate && bit(instr, 4);
    const u32 opcode = (instr >> 21) & 0xF;
    const u32 rn_index = (instr >> 16) & 0xF;
    const u32 rd = (instr >> 12) & 0xF;

    const ShifterResult op2 = immediate ? operand2_immediate(instr) : operand2_register(instr);
    const u32 rn = regs_.r[rn_index] + ((register_shift && rn_index == 15) ? 4u : 0u);
    // Flags from Rd=15 with S are replaced by the SPSR restore below.
    const bool update_flags = set_flags && rd != 15;
    const bool c = carry_flag();

    u32 result = 0;
    bool writes_rd = true;
    auto logical = [&](u32 value) {
        result = value;
        if (update_flags) set_nzc(result, op2.carry);
    };
    auto arithmetic = [&](AddResult r) {
        result = r.value;
        if (update_flags) set_nzcv(result, r.carry, r.overflow);
    };

    switch (opcode) {
        case kAnd: logical(rn & op2.value); break;
        case kEor: logical(rn ^ op2.value); break;
        case kSub: arithmetic(add_with_carry(rn, ~op2.value, true)); break;
        case kRsb: arithmetic(add_with_carry(op2.value, ~rn, true)); break;
        case kAdd: arithmetic(add_with_carry(rn, op2.value, false)); break;
        case kAdc: arithmetic(add_with_carry(rn, op2.value, c)); break;
        case kSbc: arithmetic(add_with_carry(rn, ~op2.value, c)); break;
        case kRsc: arithmetic(add_with_carry(op2.value, ~rn, c)); break;
        // Compares always set flags (S=1 is required by the decoder) and never write Rd.
        case kTst: writes_rd = false; set_nzc(rn & op2.value, op2.carry); break;
        case kTeq: writes_rd = false; set_nzc(rn ^ op2.value, op2.carry); break;
        case kCmp: {
            writes_rd = false;
            const AddResult r = add_with_carry(rn, ~op2.value, true);
            set_nzcv(r.value, r.carry, r.overflow);
            break;
        }
        case kCmn: {
            writes_rd = false;
            const AddResult r = add_with_carry(rn, op2.value, false);
            set_nzcv(r.value, r.carry, r.overflow);
            break;
        }
        case kOrr: logical(rn | op2.value); break;
        case kMov: logical(op2.value); break;
        case kBic: logical(rn & ~op2.value); break;
        default: logical(~op2.value); break;  // kMvn
    }

    u32 cycles = kS + (register_shift ? kI : 0);  // 1S (+1I for register shift)
    if (!writes_rd) return cycles;
    if (rd == 15) {
        if (set_flags) restore_cpsr_from_spsr();
        write_pc(result);
        cycles += kS + kN;  // pipeline refill: +1N +1S
    } else {
        regs_.r[rd] = result;
    }
    return cycles;
}

// --- Multiply, swap, PSR transfer --------------------------------------------

u32 Arm60::op_multiply(u32 instr) {
    const bool accumulate = bit(instr, 21);
    const bool set_flags = bit(instr, 20);
    const u32 rd = (instr >> 16) & 0xF;
    const u32 rn = (instr >> 12) & 0xF;
    const u32 rs_value = regs_.r[(instr >> 8) & 0xF];
    const u32 rm_value = regs_.r[instr & 0xF];

    u32 result = rm_value * rs_value;
    if (accumulate) result += regs_.r[rn];
    if (rd != 15) regs_.r[rd] = result;
    // ARMv3: N and Z are set, C is unpredictable (left alone), V unaffected.
    if (set_flags) set_nz(result);

    // Booth multiplier retires 2 bits of Rs per internal cycle (approximation).
    const u32 significant_bits = 32 - static_cast<u32>(std::countl_zero(rs_value));
    const u32 booth_cycles = std::max(1u, (significant_bits + 1) / 2);
    return kS + booth_cycles * kI + (accumulate ? kI : 0);
}

u32 Arm60::op_swap(u32 instr) {
    const bool byte = bit(instr, 22);
    const u32 address = regs_.r[(instr >> 16) & 0xF];
    const u32 rd = (instr >> 12) & 0xF;
    const u32 source = regs_.r[instr & 0xF];

    u32 loaded;
    if (byte) {
        loaded = bus_.read8(address);
        bus_.write8(address, static_cast<u8>(source));
    } else {
        loaded = load_word_rotated(bus_, address);
        bus_.write32(address & ~3u, source);
    }
    if (bus_.consume_abort()) return take_data_abort();
    if (rd == 15)
        write_pc(loaded);
    else
        regs_.r[rd] = loaded;
    return kS + 2 * kN + kI;
}

u32 Arm60::op_mrs(u32 instr) {
    const bool spsr = bit(instr, 22);
    const u32 rd = (instr >> 12) & 0xF;
    const Bank bank = current_bank();
    // SPSR access in User mode is unpredictable; return CPSR.
    regs_.r[rd] = (spsr && bank != kBankUser) ? regs_.spsr[bank] : regs_.cpsr;
    return 1;
}

u32 Arm60::op_msr(u32 instr) {
    const bool spsr = bit(instr, 22);
    const u32 value = bit(instr, 25) ? operand2_immediate(instr).value : regs_.r[instr & 0xF];

    // Field mask instr[19:16] = f s x c. ARMv3 encodes "CPSR_all" as f+c and
    // "CPSR_flg" as f only; this generic form handles both.
    u32 mask = 0;
    if (bit(instr, 19)) mask |= 0xFF000000;
    if (bit(instr, 18)) mask |= 0x00FF0000;
    if (bit(instr, 17)) mask |= 0x0000FF00;
    if (bit(instr, 16)) mask |= 0x000000FF;

    const Bank bank = current_bank();
    if (spsr) {
        if (bank != kBankUser) regs_.spsr[bank] = (regs_.spsr[bank] & ~mask) | (value & mask);
        return 1;
    }
    if (bank == kBankUser) mask &= 0xFF000000;  // User mode may only change the flags
    set_cpsr((regs_.cpsr & ~mask) | (value & mask));
    return 1;
}

// --- Memory transfers --------------------------------------------------------

u32 Arm60::op_single_transfer(u32 instr) {
    const bool register_offset = bit(instr, 25);
    const bool pre_index = bit(instr, 24);
    const bool up = bit(instr, 23);
    const bool byte = bit(instr, 22);
    const bool writeback = bit(instr, 21);
    const bool load = bit(instr, 20);
    const u32 rn = (instr >> 16) & 0xF;
    const u32 rd = (instr >> 12) & 0xF;

    u32 offset = instr & 0xFFF;
    if (register_offset) {
        const u32 amount = (instr >> 7) & 0x1F;
        const u32 type = (instr >> 5) & 0x3;
        offset = shift_by_immediate(type, regs_.r[instr & 0xF], amount, carry_flag()).value;
    }

    const u32 base = regs_.r[rn];
    const u32 offset_base = up ? base + offset : base - offset;
    const u32 address = pre_index ? offset_base : base;
    // Post-indexed with W set is LDRT/STRT: the access uses User-mode registers.
    const bool user_access = !pre_index && writeback;
    const bool do_writeback = !pre_index || writeback;

    if (load) {
        const u32 value = byte ? bus_.read8(address) : load_word_rotated(bus_, address);
        if (bus_.consume_abort()) return take_data_abort();  // no load, no writeback
        if (do_writeback && rn != 15) regs_.r[rn] = offset_base;  // the load wins if rn == rd
        if (user_access)
            set_user_reg(rd, value);
        else if (rd == 15)
            write_pc(value);
        else
            regs_.r[rd] = value;
        return kS + kN + kI + (rd == 15 ? kS + kN : 0);  // 1S+1N+1I (+1S+1N for PC)
    }

    u32 value = user_access ? user_reg(rd) : regs_.r[rd];
    if (rd == 15) value += 4;  // stored PC is the instruction address + 12
    if (byte)
        bus_.write8(address, static_cast<u8>(value));
    else
        bus_.write32(address & ~3u, value);
    if (bus_.consume_abort()) return take_data_abort();
    if (do_writeback && rn != 15) regs_.r[rn] = offset_base;
    return 2 * kN;
}

u32 Arm60::op_block_transfer(u32 instr) {
    const bool pre_index = bit(instr, 24);
    const bool up = bit(instr, 23);
    const bool psr_or_user = bit(instr, 22);
    const bool writeback = bit(instr, 21);
    const bool load = bit(instr, 20);
    const u32 rn = (instr >> 16) & 0xF;
    const u32 list = instr & 0xFFFF;

    const auto count = static_cast<u32>(std::popcount(list));
    if (count == 0) {
        Log::warn("ARM60: LDM/STM with empty register list at 0x{:08X} (unpredictable, skipped)", pc());
        return 1;
    }

    const u32 base = regs_.r[rn];
    const u32 new_base = up ? base + 4 * count : base - 4 * count;
    // Registers always go lowest-numbered to lowest address.
    u32 address = up ? base + (pre_index ? 4u : 0u) : new_base + (pre_index ? 0u : 4u);

    const bool loads_pc = load && bit(list, 15);
    // S bit: with LDM of PC it restores CPSR; otherwise it selects User registers.
    const bool user_bank = psr_or_user && !loads_pc;

    if (load) {
        // Read everything first so an abort leaves the registers untouched.
        std::array<u32, 16> values{};
        bool aborted = false;
        for (u32 i = 0; i < 16; ++i) {
            if (!bit(list, i)) continue;
            values[i] = bus_.read32(address);
            address += 4;
            aborted = bus_.consume_abort() || aborted;
        }
        if (aborted) return take_data_abort();

        if (writeback && rn != 15) regs_.r[rn] = new_base;  // loaded values win over writeback
        for (u32 i = 0; i < 16; ++i) {
            if (!bit(list, i)) continue;
            const u32 value = values[i];
            if (i == 15) {
                if (psr_or_user) restore_cpsr_from_spsr();
                write_pc(value);
            } else if (user_bank) {
                set_user_reg(i, value);
            } else {
                regs_.r[i] = value;
            }
        }
        return count * kS + kN + kI + (loads_pc ? kS + kN : 0);  // nS+1N+1I (+1S+1N for PC)
    }

    const u32 lowest = static_cast<u32>(std::countr_zero(list));
    bool aborted = false;
    for (u32 i = 0; i < 16; ++i) {
        if (!bit(list, i)) continue;
        u32 value = user_bank ? user_reg(i) : regs_.r[i];
        if (i == 15) value += 4;  // stored PC is the instruction address + 12
        // ARM6: the base is written back after the first transfer, so a base
        // that is not the lowest listed register is stored as the new value.
        if (i == rn && writeback && i != lowest) value = new_base;
        bus_.write32(address, value);
        address += 4;
        aborted = bus_.consume_abort() || aborted;
    }
    if (aborted) return take_data_abort();
    if (writeback && rn != 15) regs_.r[rn] = new_base;
    return (count - 1) * kS + 2 * kN;  // (n-1)S+2N
}

// --- Branches and exceptions -------------------------------------------------

u32 Arm60::op_branch(u32 instr) {
    const auto offset = static_cast<u32>(static_cast<s32>(instr << 8) >> 6);  // sign-extend, *4
    if (bit(instr, 24)) regs_.r[14] = pc() + 4;  // BL: return to the next instruction
    write_pc(regs_.r[15] + offset);
    return 2 * kS + kN;
}

u32 Arm60::take_data_abort() {
    // Return address is the aborted instruction + 8 (handlers use SUBS pc, lr, #8).
    enter_exception(Mode::Abort, vector::kDataAbort, pc() + 8);
    return 2 * kS + kN;
}

u32 Arm60::op_swi(u32 /*instr*/) {
    enter_exception(Mode::Supervisor, vector::kSwi, pc() + 4);
    return 2 * kS + kN;
}

u32 Arm60::op_undefined(u32 instr) {
    const std::size_t slot = decode_index(instr);
    if (!warned_undefined_.test(slot)) {
        warned_undefined_.set(slot);
        Log::warn("ARM60: undefined instruction 0x{:08X} at 0x{:08X}, taking the undefined trap",
                  instr, pc());
    }
    enter_exception(Mode::Undefined, vector::kUndefined, pc() + 4);
    return 2 * kS + kN;
}

}  // namespace core::cpu
