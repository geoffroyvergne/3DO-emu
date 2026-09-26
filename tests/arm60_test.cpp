// Minimal self-contained tests for the ARM60 data-processing path.
#include <vector>

#include "core/bus/bus.hpp"
#include "core/cpu/arm60.hpp"
#include "test_common.hpp"

namespace {

// Flat big-endian RAM, standing in for the real 3DO bus.
class FlatBus final : public core::Bus {
public:
    explicit FlatBus(std::size_t size) : mem_(size) {}

    u32 read32(u32 address) override {
        const std::size_t a = address & ~3u;
        return (u32{mem_[a]} << 24) | (u32{mem_[a + 1]} << 16) | (u32{mem_[a + 2]} << 8) |
               u32{mem_[a + 3]};
    }
    u8 read8(u32 address) override { return mem_[address]; }
    void write8(u32 address, u8 value) override { mem_[address] = value; }
    void write32(u32 address, u32 value) override {
        const std::size_t a = address & ~3u;
        mem_[a] = static_cast<u8>(value >> 24);
        mem_[a + 1] = static_cast<u8>(value >> 16);
        mem_[a + 2] = static_cast<u8>(value >> 8);
        mem_[a + 3] = static_cast<u8>(value);
    }

private:
    std::vector<u8> mem_;
};

// --- Instruction encoders -------------------------------------------------
constexpr u32 kAL = 0xE, kEQ = 0x0, kNE = 0x1;
constexpr u32 kSUB = 0x2, kADD = 0x4, kMOV = 0xD;
constexpr u32 kLSL = 0, kLSR = 1, kROR = 3;

constexpr u32 dp_imm(u32 cond, u32 op, bool s, u32 rn, u32 rd, u32 rot, u32 imm8) {
    return (cond << 28) | (1u << 25) | (op << 21) | (u32{s} << 20) | (rn << 16) | (rd << 12) |
           (rot << 8) | imm8;
}
constexpr u32 dp_shift_imm(u32 cond, u32 op, bool s, u32 rn, u32 rd, u32 rm, u32 type, u32 amt) {
    return (cond << 28) | (op << 21) | (u32{s} << 20) | (rn << 16) | (rd << 12) | (amt << 7) |
           (type << 5) | rm;
}
constexpr u32 dp_shift_reg(u32 cond, u32 op, bool s, u32 rn, u32 rd, u32 rm, u32 type, u32 rs) {
    return (cond << 28) | (op << 21) | (u32{s} << 20) | (rn << 16) | (rd << 12) | (rs << 8) |
           (type << 5) | (1u << 4) | rm;
}

namespace psr = core::cpu::psr;

struct Fixture {
    FlatBus bus{0x1000};
    core::cpu::Arm60 cpu{bus};

    // Loads one instruction at the current PC and executes it.
    u32 run(u32 instr) {
        bus.write32(cpu.pc(), instr);
        return cpu.step();
    }
    u32 flags() const { return cpu.regs().cpsr & (psr::kN | psr::kZ | psr::kC | psr::kV); }
};

void test_mov_immediate() {
    Fixture f;
    f.run(dp_imm(kAL, kMOV, true, 0, 0, 4, 0xFF));  // MOVS r0, #0xFF000000
    CHECK_EQ(f.cpu.regs().r[0], 0xFF000000u);
    CHECK_EQ(f.flags(), psr::kN | psr::kC);          // rotated imm: C = bit 31
    CHECK_EQ(f.cpu.pc(), 4u);
}

void test_add_sub_flags() {
    Fixture f;
    auto& r = f.cpu.regs().r;

    r[1] = 0x7FFFFFFF; r[2] = 1;
    f.run(dp_shift_imm(kAL, kADD, true, 1, 0, 2, kLSL, 0));  // ADDS r0, r1, r2
    CHECK_EQ(r[0], 0x80000000u);
    CHECK_EQ(f.flags(), psr::kN | psr::kV);

    r[1] = 0xFFFFFFFF; r[2] = 1;
    f.run(dp_shift_imm(kAL, kADD, true, 1, 0, 2, kLSL, 0));  // ADDS: unsigned carry, zero
    CHECK_EQ(r[0], 0u);
    CHECK_EQ(f.flags(), psr::kZ | psr::kC);

    r[1] = 5; r[2] = 5;
    f.run(dp_shift_imm(kAL, kSUB, true, 1, 0, 2, kLSL, 0));  // SUBS r0, r1, r2
    CHECK_EQ(r[0], 0u);
    CHECK_EQ(f.flags(), psr::kZ | psr::kC);                  // no borrow -> C set

    r[1] = 3; r[2] = 5;
    f.run(dp_shift_imm(kAL, kSUB, true, 1, 0, 2, kLSL, 0));
    CHECK_EQ(r[0], 0xFFFFFFFEu);
    CHECK_EQ(f.flags(), psr::kN);                            // borrow -> C clear

    r[1] = 0x80000000; r[2] = 1;
    f.run(dp_shift_imm(kAL, kSUB, true, 1, 0, 2, kLSL, 0));  // signed overflow
    CHECK_EQ(r[0], 0x7FFFFFFFu);
    CHECK_EQ(f.flags(), psr::kC | psr::kV);

    const u32 before = f.flags();
    f.run(dp_imm(kAL, kADD, false, 1, 0, 0, 1));             // ADD without S keeps flags
    CHECK_EQ(f.flags(), before);
}

void test_immediate_shifts() {
    Fixture f;
    auto& r = f.cpu.regs().r;
    r[1] = 0x80000001;

    f.run(dp_shift_imm(kAL, kMOV, true, 0, 0, 1, kLSL, 4));  // MOVS r0, r1, LSL #4
    CHECK_EQ(r[0], 0x00000010u);
    CHECK_EQ(f.flags(), 0u);                                 // last bit out = r1[28] = 0

    r[1] = 0x10000000;
    f.run(dp_shift_imm(kAL, kMOV, true, 0, 0, 1, kLSL, 4));  // shifts r1[28]=1 into C
    CHECK_EQ(r[0], 0u);
    CHECK_EQ(f.flags(), psr::kZ | psr::kC);
}

void test_immediate_shift_special_cases() {
    Fixture f;
    auto& r = f.cpu.regs().r;
    r[1] = 0x80000001;

    f.run(dp_shift_imm(kAL, kMOV, true, 0, 0, 1, kLSR, 0));  // LSR #0 means LSR #32
    CHECK_EQ(r[0], 0u);
    CHECK_EQ(f.flags(), psr::kZ | psr::kC);

    f.run(dp_shift_imm(kAL, kMOV, true, 0, 0, 1, kROR, 0));  // ROR #0 means RRX (C was 1)
    CHECK_EQ(r[0], 0xC0000000u);
    CHECK_EQ(f.flags(), psr::kN | psr::kC);
}

void test_register_shift() {
    Fixture f;
    auto& r = f.cpu.regs().r;
    r[1] = 0x1; r[6] = 0x104;                                  // only Rs[7:0] used -> 4

    const u32 cycles = f.run(dp_shift_reg(kAL, kMOV, false, 0, 5, 1, kLSL, 6));
    CHECK_EQ(r[5], 0x10u);
    CHECK_EQ(cycles, 2u);                                      // +1 internal cycle

    r[6] = 32;
    f.run(dp_shift_reg(kAL, kMOV, true, 0, 5, 1, kLSL, 6));   // LSL by 32: result 0, C = bit 0
    CHECK_EQ(r[5], 0u);
    CHECK_EQ(f.flags(), psr::kZ | psr::kC);

    r[6] = 33;
    f.run(dp_shift_reg(kAL, kMOV, true, 0, 5, 1, kLSL, 6));   // LSL by >32: C = 0
    CHECK_EQ(f.flags(), psr::kZ);
}

void test_pc_operand_and_branch() {
    Fixture f;
    auto& r = f.cpu.regs().r;
    f.cpu.set_pc(0x40);

    f.run(dp_imm(kAL, kADD, false, 15, 7, 0, 0));             // ADD r7, pc, #0
    CHECK_EQ(r[7], 0x48u);                                     // PC reads +8

    r[2] = 0;
    f.run(dp_shift_reg(kAL, kADD, false, 15, 8, 2, kLSL, 2));  // ADD r8, pc, r2, LSL r2
    CHECK_EQ(r[8], 0x44u + 12u);                               // PC reads +12

    const u32 cycles = f.run(dp_imm(kAL, kMOV, false, 0, 15, 0, 0x80));  // MOV pc, #0x80
    CHECK_EQ(f.cpu.pc(), 0x80u);
    CHECK_EQ(cycles, 3u);
}

void test_conditions() {
    Fixture f;
    auto& r = f.cpu.regs().r;
    f.cpu.regs().cpsr |= psr::kZ;

    f.run(dp_imm(kEQ, kMOV, false, 0, 0, 0, 1));   // MOVEQ r0, #1 -> executes
    f.run(dp_imm(kNE, kMOV, false, 0, 1, 0, 1));   // MOVNE r1, #1 -> skipped
    CHECK_EQ(r[0], 1u);
    CHECK_EQ(r[1], 0u);
    CHECK_EQ(f.cpu.pc(), 8u);
}

void test_spsr_restore_and_banking() {
    Fixture f;
    auto& regs = f.cpu.regs();
    regs.r[13] = 0x5555;                                        // SVC stack pointer
    regs.spsr[core::cpu::kBankSvc] = static_cast<u32>(core::cpu::Mode::User) | psr::kC;
    regs.r[14] = 0x100;

    f.run(dp_shift_imm(kAL, kMOV, true, 0, 15, 14, kLSL, 0));  // MOVS pc, lr
    CHECK_EQ(static_cast<u32>(f.cpu.mode()), static_cast<u32>(core::cpu::Mode::User));
    CHECK_EQ(f.flags(), psr::kC);
    CHECK_EQ(f.cpu.pc(), 0x100u);
    CHECK_EQ(regs.r[13], 0u);                                   // user SP, not SVC's
    CHECK_EQ(regs.r13_r14[core::cpu::kBankSvc][0], 0x5555u);
}

void test_execute_cycles() {
    Fixture f;
    for (u32 addr = 0; addr < 0x100; addr += 4) f.bus.write32(addr, dp_imm(kAL, kADD, false, 0, 0, 0, 1));
    const u32 executed = f.cpu.execute_cycles(10);
    CHECK_EQ(executed, 10u);
    CHECK_EQ(f.cpu.regs().r[0], 10u);
}

// --- Full ARMv3 coverage -----------------------------------------------------

void test_branches() {
    Fixture f;
    f.cpu.set_pc(0x100);
    f.run(0xEA000002);                 // B +2 words -> 0x100 + 8 + 8
    CHECK_EQ(f.cpu.pc(), 0x110u);
    f.run(0xEBFFFFFE);                 // BL -2 words -> 0x110 + 8 - 8 = itself
    CHECK_EQ(f.cpu.pc(), 0x110u);
    CHECK_EQ(f.cpu.regs().r[14], 0x114u);
    f.cpu.regs().cpsr |= psr::kZ;
    f.run(0x1A000010);                 // BNE: not taken
    CHECK_EQ(f.cpu.pc(), 0x114u);
}

void test_compares_and_logic() {
    Fixture f;
    auto& r = f.cpu.regs().r;
    r[0] = 5;
    f.run(0xE3500005);                 // CMP r0, #5
    CHECK_EQ(f.flags(), psr::kZ | psr::kC);
    f.run(0xE3500006);                 // CMP r0, #6 -> negative, borrow
    CHECK_EQ(f.flags(), psr::kN);
    r[1] = 0xF0;
    f.run(0xE2012033);                 // AND r2, r1, #0x33
    CHECK_EQ(r[2], 0x30u);
    f.run(0xE3C12010);                 // BIC r2, r1, #0x10
    CHECK_EQ(r[2], 0xE0u);
    f.run(0xE1E02001);                 // MVN r2, r1
    CHECK_EQ(r[2], 0xFFFFFF0Fu);
    f.cpu.regs().cpsr |= psr::kC;
    r[3] = 1; r[4] = 2;
    f.run(0xE0A35004);                 // ADC r5, r3, r4 -> 1 + 2 + C
    CHECK_EQ(r[5], 4u);
    f.run(0xE0635004);                 // RSB r5, r3, r4 -> 2 - 1
    CHECK_EQ(r[5], 1u);
}

void test_single_transfers() {
    Fixture f;
    auto& r = f.cpu.regs().r;
    f.bus.write32(0x800, 0x11223344);
    f.bus.write32(0x804, 0xAABBCCDD);
    r[1] = 0x800;

    f.run(0xE5910000);                 // LDR r0, [r1]
    CHECK_EQ(r[0], 0x11223344u);
    f.run(0xE5B10004);                 // LDR r0, [r1, #4]!
    CHECK_EQ(r[0], 0xAABBCCDDu);
    CHECK_EQ(r[1], 0x804u);
    f.run(0xE4910004);                 // LDR r0, [r1], #4 (post-index)
    CHECK_EQ(r[0], 0xAABBCCDDu);
    CHECK_EQ(r[1], 0x808u);

    r[1] = 0x800;
    f.run(0xE5D10001);                 // LDRB r0, [r1, #1] -> big-endian byte 1
    CHECK_EQ(r[0], 0x22u);
    r[1] = 0x801;
    f.run(0xE5910000);                 // LDR unaligned: word rotated right by 8
    CHECK_EQ(r[0], 0x44112233u);

    r[0] = 0xCAFEBABE; r[1] = 0x810;
    f.run(0xE5010004);                 // STR r0, [r1, #-4]
    CHECK_EQ(f.bus.read32(0x80C), 0xCAFEBABEu);
    CHECK_EQ(r[1], 0x810u);            // no writeback
    f.run(0xE5C10000);                 // STRB r0, [r1]
    CHECK_EQ(f.bus.read8(0x810), 0xBEu);

    r[1] = 0x800; r[2] = 1;
    f.run(0xE7910102);                 // LDR r0, [r1, r2, LSL #2]
    CHECK_EQ(r[0], 0xAABBCCDDu);

    f.cpu.set_pc(0x200);
    r[1] = 0x900;
    f.run(0xE581F000);                 // STR pc, [r1] -> instruction address + 12
    CHECK_EQ(f.bus.read32(0x900), 0x20Cu);

    f.bus.write32(0x904, 0x340);
    r[1] = 0x904;
    f.run(0xE591F000);                 // LDR pc, [r1] -> jump
    CHECK_EQ(f.cpu.pc(), 0x340u);
}

void test_block_transfers() {
    Fixture f;
    auto& r = f.cpu.regs().r;
    r[13] = 0x800;
    r[0] = 10; r[1] = 11; r[2] = 12; r[14] = 0x77;

    f.run(0xE92D4007);                 // STMFD sp!, {r0-r2, lr}
    CHECK_EQ(r[13], 0x7F0u);
    CHECK_EQ(f.bus.read32(0x7F0), 10u);  // lowest register at lowest address
    CHECK_EQ(f.bus.read32(0x7FC), 0x77u);

    f.run(0xE8BD0070);                 // LDMFD sp!, {r4-r6}
    CHECK_EQ(r[4], 10u);
    CHECK_EQ(r[6], 12u);
    CHECK_EQ(r[13], 0x7FCu);

    r[13] = 0x800;
    f.bus.write32(0x800, 0x3C0);
    f.run(0xE89D8000);                 // LDMIA sp, {pc}
    CHECK_EQ(f.cpu.pc(), 0x3C0u);
    CHECK_EQ(r[13], 0x800u);
}

void test_multiply_and_swap() {
    Fixture f;
    auto& r = f.cpu.regs().r;
    r[1] = 7; r[2] = 6; r[3] = 100;
    f.run(0xE0000291);                 // MUL r0, r1, r2
    CHECK_EQ(r[0], 42u);
    f.run(0xE0303291);                 // MLAS r0, r1, r2, r3
    CHECK_EQ(r[0], 142u);
    CHECK_EQ(f.flags() & (psr::kN | psr::kZ), 0u);

    f.bus.write32(0x800, 0x55);
    r[1] = 0x99; r[2] = 0x800;
    f.run(0xE1020091);                 // SWP r0, r1, [r2]
    CHECK_EQ(r[0], 0x55u);
    CHECK_EQ(f.bus.read32(0x800), 0x99u);
}

void test_psr_transfers_and_mode_switch() {
    Fixture f;
    auto& regs = f.cpu.regs();
    f.run(0xE10F0000);                 // MRS r0, CPSR
    CHECK_EQ(regs.r[0] & psr::kModeMask, static_cast<u32>(core::cpu::Mode::Supervisor));

    regs.r[13] = 0x5000;               // SVC stack
    regs.r[0] = (regs.r[0] & ~psr::kModeMask) | static_cast<u32>(core::cpu::Mode::Irq);
    f.run(0xE129F000);                 // MSR CPSR_all, r0 -> IRQ mode
    CHECK_EQ(static_cast<u32>(f.cpu.mode()), static_cast<u32>(core::cpu::Mode::Irq));
    CHECK_EQ(regs.r[13], 0u);          // IRQ bank
    CHECK_EQ(regs.r13_r14[core::cpu::kBankSvc][0], 0x5000u);

    f.run(0xE328F20F);                 // MSR CPSR_flg, #0xF0000000
    CHECK_EQ(f.flags(), psr::kN | psr::kZ | psr::kC | psr::kV);
    CHECK_EQ(static_cast<u32>(f.cpu.mode()), static_cast<u32>(core::cpu::Mode::Irq));
}

void test_exceptions() {
    Fixture f;
    auto& regs = f.cpu.regs();
    f.cpu.set_pc(0x100);
    const u32 old_cpsr = regs.cpsr;
    f.run(0xEF000123);                 // SWI
    CHECK_EQ(f.cpu.pc(), 0x08u);
    CHECK_EQ(regs.r[14], 0x104u);
    CHECK_EQ(regs.spsr[core::cpu::kBankSvc], old_cpsr);

    f.cpu.set_pc(0x200);
    f.run(0xE6000010);                 // undefined instruction
    CHECK_EQ(f.cpu.pc(), 0x04u);
    CHECK_EQ(static_cast<u32>(f.cpu.mode()), static_cast<u32>(core::cpu::Mode::Undefined));
    CHECK_EQ(regs.r[14], 0x204u);
}

void test_irq_entry_and_return() {
    Fixture f;
    auto& regs = f.cpu.regs();
    regs.cpsr &= ~psr::kIrqDisable;
    f.cpu.set_pc(0x300);
    f.bus.write32(0x18, 0xE25EF004);   // IRQ vector: SUBS pc, lr, #4

    f.cpu.set_irq_line(true);
    f.cpu.step();                      // takes the interrupt instead of executing 0x300
    CHECK_EQ(f.cpu.pc(), 0x18u);
    CHECK_EQ(static_cast<u32>(f.cpu.mode()), static_cast<u32>(core::cpu::Mode::Irq));
    CHECK_EQ((regs.cpsr & psr::kIrqDisable) != 0, true);

    f.cpu.set_irq_line(false);
    f.cpu.step();                      // SUBS pc, lr, #4
    CHECK_EQ(f.cpu.pc(), 0x300u);
    CHECK_EQ(static_cast<u32>(f.cpu.mode()), static_cast<u32>(core::cpu::Mode::Supervisor));

    regs.cpsr |= psr::kIrqDisable;     // masked: executes normally
    f.cpu.set_irq_line(true);
    f.bus.write32(0x300, dp_imm(kAL, kMOV, false, 0, 0, 0, 9));
    f.cpu.step();
    CHECK_EQ(regs.r[0], 9u);
}

}  // namespace

int main() {
    test_mov_immediate();
    test_add_sub_flags();
    test_immediate_shifts();
    test_immediate_shift_special_cases();
    test_register_shift();
    test_pc_operand_and_branch();
    test_conditions();
    test_spsr_restore_and_banking();
    test_execute_cycles();
    test_branches();
    test_compares_and_logic();
    test_single_transfers();
    test_block_transfers();
    test_multiply_and_swap();
    test_psr_transfers_and_mode_switch();
    test_exceptions();
    test_irq_entry_and_return();

    return report("arm60_test");
}
