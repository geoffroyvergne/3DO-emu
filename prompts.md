# Step 0 : 

Use our emulator_dev and 3do_specs skills for the next prompts.

# Step 1: Overall structure and SDL2 loop

I want to develop a 3DO console emulator from scratch using C++20, CMake, and SDL2 for graphics and inputs. Act as an expert in console emulation and software architecture. Please provide a clean project directory structure and a CMakeLists.txt file that links SDL2. Also, provide a basic main.cpp demonstrating a standard emulation loop (Init, Process Frame, Render, Handle SDL Events) targeting a 60Hz refresh rate.

# Step 2: The ARM60 CPU (BASIC Interpreter)

The 3DO uses an ARM60 CPU (ARMv3 architecture, 32-bit). I need to implement the core CPU class. Create a C++ structure for the ARM60 registers (including CPSR/SPSR) and the skeleton of the CPU class. Include a function executeCycles(uint32_t cycles) and show how to decode and implement three core data processing instructions: ADD, SUB, and MOV (handling immediate values and register shifts).

# Step 3: Memory Mapping (MMIO) and BIOS

Now let's build the Memory Map for the 3DO. The system has 2MB of Main RAM, 1MB of VRAM, and a 1.25MB ROM (BIOS). Create a Memory class in C++ that manages these regions. Implement basic read32(uint32_t address) and write32(uint32_t address, uint32_t value) methods. Include placeholder logic for Memory Mapped I/O (MMIO) ranges used by the hardware components.

# Step 4 : CD-ROM drive (Opera File System)

To boot games, the 3DO uses a custom CD-ROM file system called Opera FS. Write a C++ parser that can open a 3DO .iso or .bin file, read the volume descriptor, navigate the directory structure, and locate the boot file (launchme). Provide clean code to log the contents of the ISO root directory to the console.

# Step 5 : Cel Engine (Graphic Engine)

The 3DO graphics processor is the Cel Engine, which renders projectable mathematical sprites called 'Cels'. Assuming we have decoded a Cel control block (CCB) from memory, write a C++ function using SDL2 or a software rasterizer to draw a single 4-sided warped texture Cel into a frame buffer. Explain how preamble bits dictate transparency and blending.

# Step 6 : Inputs (3DO Control Pad)

The 3DO controller uses a daisy-chain serial protocol. In hardware, inputs are read through specific MMIO registers. Create an input management system that intercepts these MMIO reads and maps standard SDL2 controller/keyboard events to the 3DO button bitmask (Up, Down, Left, Right, A, B, C, P, X, L, R)."