#pragma once

#include <array>
#include <functional>
#include <utility>

#include "common/types.hpp"

namespace core::dsp {

class AudioDma;

// DSPP: the 3DO's audio signal processor. The ARM loads a program into its
// code memory ("N memory") and data into its "I memory"; the DSP then runs
// the program once per audio sample (44.1 kHz) until it executes `sleep`,
// leaving the stereo output in I-memory 0x3FE/0x3FF.
//
// 16-bit instructions: bit 15 set = control (branches, moves, sleep...),
// clear = ALU op with a multiplier, an ALU and a barrel shifter, followed by
// operand words. Semantics follow Opera's opera_dsp.c (32-bit ALU build).
class Dspp {
public:
    static constexpr u32 kSampleRate = 44100;

    explicit Dspp(AudioDma& fifos) : fifos_(fifos) { build_tables(); init(); }

    void init();    // power-on state: code memory full of `sleep`
    void reset();   // CLIO +0x17E8 (per-sample state)

    void set_running(bool running) { running_ = running; }
    [[nodiscard]] bool running() const { return running_; }

    // Runs the program for one sample; returns (right << 16) | left.
    u32 run_sample();

    // True (once) when the program wrote its interrupt register.
    bool take_interrupt() { return std::exchange(gen_fiq_, false); }

    // Diagnostics: DSP reads of each input FIFO's data and status since the last call.
    struct FifoCounters {
        std::array<u32, 16> data{};
        std::array<u32, 16> status{};
    };
    FifoCounters take_fifo_counters() { return std::exchange(counters_, {}); }

    // ARM side (through CLIO).
    void write_code(u32 address, u16 value) { nmem_[address & 0x3FF] = value; }
    void write_input(u32 address, u16 value);   // "EI" / I memory
    [[nodiscard]] u16 read_output(u32 address) const;  // "EO" / I memory
    void write_semaphore(u32 value);
    [[nodiscard]] u32 read_semaphore() const { return (u32{sema4_status_} << 16) | sema4_data_; }

private:
    struct DecodedAlu {
        u8 requests;  // operand requests: bit0 BS, 1 ALU2, 2 ALU1, 3 MULT2, 4 MULT1
        u8 shift;     // barrel shifter mode (5 bits)
    };
    static constexpr u8 kReqBs = 1, kReqAlu2 = 2, kReqAlu1 = 4, kReqMult2 = 8, kReqMult1 = 16;

    void build_tables();
    [[nodiscard]] u16 register_address(u32 reg) const;
    u16 read(u32 address);
    void write(u32 address, u16 value);
    u16 load_operand1();
    void load_operands(u32 count);
    [[nodiscard]] u16 noise();

    AudioDma& fifos_;

    std::array<u16, 2048> nmem_{};
    std::array<u16, 1024> imem_{};
    std::array<std::array<u16, 16>, 8> reg_conv_{};
    std::array<DecodedAlu, 0x8000> alu_decode_{};
    std::array<std::array<bool, 32>, 32> branch_table_{};
    std::array<bool, 16> cpu_supply_{};

    // Registers.
    u16 pc_ = 0;
    u16 audio_out_status_ = 0;
    u16 sema4_status_ = 0;
    u16 sema4_data_ = 0;
    s32 dspp_cnt_ = 0;
    s32 dspp_rld_ = 0;
    u16 int_value_ = 0;
    u32 rbase4_ = 0;
    u32 reg_map_ = 0;
    u8 op_mask_ = 0xFF;  // inverted "op mask": requests kept
    bool running_ = false;
    bool gen_fiq_ = false;

    // Operand latches for the current ALU instruction.
    s16 mult1_ = 0, mult2_ = 0, alu1_ = 0, alu2_ = 0;
    s32 bs_ = 0;
    u8 requests_ = 0;
    u16 writeback_ = 0;
    u32 rng_ = 0x2545F491;
    FifoCounters counters_;
};

}  // namespace core::dsp
