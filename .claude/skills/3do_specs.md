# Skill: 3DO Hardware Specifications Reference

## System Architecture Overview
- **CPU:** ARM60 (ARMv3 Architecture, 32-bit RISC), running at 12.5 MHz.
- **Endianness:** Configured and running in **Big-Endian** mode.
- **Graphics (Cel Engine):** Two custom video processors (VGP). Renders "Cels" (projectable, warped, mathematical sprites) instead of a standard polygon pipeline.
- **Audio:** Custom 16-bit DSP running at 44.1 kHz.
- **Storage:** 2X CD-ROM drive using the custom Opera File System (Opera FS).

## Memory Map (Core Boundaries)
- **Main System RAM:** 2 MB (`0x00000000` to `0x001FFFFF`)
- **VRAM (Video RAM):** 1 MB (`0x00200000` to `0x002FFFFF`)
- **ROM (BIOS/OS):** 1 MB or 2 MB (Typically mapped at `0x03000000` or handled by the system reset vector at `0x00000000` upon boot configuration).
- *Note: Exact MMIO register definitions must be parsed iteratively during component creation.*

## ARM60 Core Requirements
- **Registers:** 16 general-purpose 32-bit registers (R0-R15), where R15 is the Program Counter (PC). 
- **Status Registers:** CPSR (Current Program Status Register) and SPSR (Saved Program Status Register for exceptions).
- **Execution:** Needs an interpreter capable of handling conditional execution (the top 4 bits of every ARM instruction dictate if the instruction executes).

## CD-ROM Format (Opera FS)
- Sector size: 2048 bytes.
- Volume Descriptor is located at Sector 0.
- Directory entries contain metadata, file flags, and block extents pointing to game assets and the primary executable (`launchme`).
