#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <optional>
#include <vector>

#include "common/types.hpp"

namespace core {
class Bus;
}

namespace core::cpu {

// ARMv3 32-bit processor modes (CPSR[4:0]). ARMv3 has no System mode.
enum class Mode : u32 {
    User = 0x10,
    Fiq = 0x11,
    Irq = 0x12,
    Supervisor = 0x13,
    Abort = 0x17,
    Undefined = 0x1B,
};

namespace psr {
inline constexpr u32 kN = 1u << 31;
inline constexpr u32 kZ = 1u << 30;
inline constexpr u32 kC = 1u << 29;
inline constexpr u32 kV = 1u << 28;
inline constexpr u32 kIrqDisable = 1u << 7;
inline constexpr u32 kFiqDisable = 1u << 6;
inline constexpr u32 kModeMask = 0x1F;
}  // namespace psr

// Exception vectors (32-bit configuration).
namespace vector {
inline constexpr u32 kReset = 0x00;
inline constexpr u32 kUndefined = 0x04;
inline constexpr u32 kSwi = 0x08;
inline constexpr u32 kPrefetchAbort = 0x0C;
inline constexpr u32 kDataAbort = 0x10;
inline constexpr u32 kIrq = 0x18;
inline constexpr u32 kFiq = 0x1C;
}  // namespace vector

// Register bank selector: each non-user mode has its own R13/R14/SPSR,
// FIQ additionally banks R8-R12.
enum Bank : std::size_t { kBankUser, kBankFiq, kBankIrq, kBankSvc, kBankAbort, kBankUndef, kBankCount };

struct Registers {
    // Active view of R0-R15 for the current mode. R15 always holds
    // (address of executing instruction + 8), mirroring the ARM pipeline.
    std::array<u32, 16> r{};
    u32 cpsr = 0;
    std::array<u32, kBankCount> spsr{};  // [kBankUser] unused

    // Storage for registers not currently mapped into `r`.
    std::array<u32, 5> r8_r12_user{};
    std::array<u32, 5> r8_r12_fiq{};
    std::array<std::array<u32, 2>, kBankCount> r13_r14{};
};

// ARM60 interpreter: the full ARMv3 instruction set (no Thumb, no halfword
// transfers, no long multiply), exceptions and the IRQ/FIQ inputs.
class Arm60 {
public:
    explicit Arm60(Bus& bus);

    void reset();

    // Runs whole instructions until at least `cycles` have elapsed. Returns the
    // cycles actually consumed (may overshoot); the caller carries the difference.
    u32 execute_cycles(u32 cycles);

    // Executes a single instruction (or takes a pending interrupt), returns its cycles.
    u32 step();

    // Interrupt inputs, level-sensitive; taken when the matching CPSR mask bit is clear.
    void set_irq_line(bool asserted) { irq_line_ = asserted; }
    void set_fiq_line(bool asserted) { fiq_line_ = asserted; }

    [[nodiscard]] Registers& regs() { return regs_; }
    [[nodiscard]] const Registers& regs() const { return regs_; }

    // Address of the next instruction to execute.
    [[nodiscard]] u32 pc() const { return regs_.r[15] - 8; }
    void set_pc(u32 address) { regs_.r[15] = (address & ~3u) + 8; }

    [[nodiscard]] Mode mode() const { return static_cast<Mode>(regs_.cpsr & psr::kModeMask); }

    // Writes CPSR, swapping banked registers if the mode changes.
    void set_cpsr(u32 value);

    // Debugging: remembers the last kTraceSize control transfers (from, to).
    struct Transfer {
        u32 from;
        u32 to;
    };
    static constexpr std::size_t kTraceSize = 256;
    void set_branch_trace(bool enabled) { trace_enabled_ = enabled; }
    // Oldest first.
    [[nodiscard]] std::vector<Transfer> recent_transfers() const;

    // Debugging: snapshot the registers the first time `address` executes.
    void set_watch_pc(u32 address) { watch_pc_ = address; watch_hit_.reset(); }
    [[nodiscard]] const std::optional<Registers>& watch_snapshot() const { return watch_hit_; }

private:
    using Handler = u32 (Arm60::*)(u32 instr);

    struct ShifterResult {
        u32 value;
        bool carry;
    };

    // Decode on instr[27:20] and instr[7:4]: 4096 entries cover every ARMv3 class.
    static constexpr std::size_t kDecodeTableSize = 4096;
    static constexpr std::size_t decode_index(u32 instr) {
        return ((instr >> 16) & 0xFF0) | ((instr >> 4) & 0xF);
    }
    static std::array<Handler, kDecodeTableSize> build_decode_table();
    static const std::array<Handler, kDecodeTableSize> kDecodeTable;

    [[nodiscard]] bool condition_passed(u32 cond) const;
    [[nodiscard]] bool carry_flag() const { return (regs_.cpsr & psr::kC) != 0; }
    void set_nzcv(u32 result, bool carry, bool overflow);
    void set_nzc(u32 result, bool carry);
    void set_nz(u32 result);

    [[nodiscard]] ShifterResult operand2_immediate(u32 instr) const;
    [[nodiscard]] ShifterResult operand2_register(u32 instr) const;

    // User-bank register access, for LDM/STM with S and LDRT/STRT.
    [[nodiscard]] u32 user_reg(u32 index) const;
    void set_user_reg(u32 index, u32 value);
    [[nodiscard]] Bank current_bank() const;

    void write_pc(u32 address);
    void restore_cpsr_from_spsr();
    void enter_exception(Mode mode, u32 vector_address, u32 return_address);
    u32 take_data_abort();

    // Instruction handlers.
    u32 op_data_processing(u32 instr);
    u32 op_multiply(u32 instr);
    u32 op_swap(u32 instr);
    u32 op_mrs(u32 instr);
    u32 op_msr(u32 instr);
    u32 op_single_transfer(u32 instr);
    u32 op_block_transfer(u32 instr);
    u32 op_branch(u32 instr);
    u32 op_swi(u32 instr);
    u32 op_undefined(u32 instr);

    Bus& bus_;
    Registers regs_;
    bool pipeline_flushed_ = false;
    bool irq_line_ = false;
    bool fiq_line_ = false;
    std::bitset<kDecodeTableSize> warned_undefined_;  // log each decode slot once
    bool trace_enabled_ = false;
    std::array<Transfer, kTraceSize> trace_{};
    std::size_t trace_count_ = 0;
    std::optional<u32> watch_pc_;
    std::optional<Registers> watch_hit_;
};

}  // namespace core::cpu
