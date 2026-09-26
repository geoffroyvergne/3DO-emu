# 3DO-emu

A Panasonic / 3DO Interactive Multiplayer emulator written from scratch in C++20, with an SDL2 frontend.

It boots the real BIOS (PAL or NTSC), reads game discs (`.iso`, `.bin`, `.chd`) and runs commercial games with video, sound and controller input. Tested so far: Doom, Total Eclipse, Wing Commander III and Wolfenstein 3D.

---

## Building

### Requirements

- A C++20 compiler (Apple Clang 15+, GCC 13+, or MSVC 2022), which must support `<format>`
- CMake 3.20 or newer
- SDL2 (development package)
- Git and network access on the first configure. CMake downloads [libchdr](https://github.com/rtissera/libchdr) (pinned commit) through `FetchContent` for CHD support.

Installing SDL2:

```sh
brew install sdl2 cmake          # macOS
sudo apt install libsdl2-dev     # Debian / Ubuntu
```

### Compile

```sh
cmake -S . -B build                         # defaults to RelWithDebInfo
cmake --build build -j
```

The default build type is `RelWithDebInfo` because a `Debug` build is too slow to run the BIOS at full speed. To build a fully optimised binary:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
```

The build produces:

| Output | Description |
|---|---|
| `build/3do-emu` | The emulator (SDL2 window, audio, input) |
| `build/opera-ls` | Lists the volume label and a directory of a disc image |
| `build/libemu_core.a` | The emulation core, with no SDL dependency |
| `build/*_test` | Headless unit tests |

### Run the tests

```sh
ctest --test-dir build --output-on-failure
```

There are seven suites: `arm60`, `memory`, `opera_fs`, `cel`, `input`, `system` and `devices`.

---

## Running

```sh
./build/3do-emu [--pal | --ntsc] [--mmio-report] <bios.bin> [disc.iso | disc.bin | disc.chd]
```

Examples:

```sh
# Boot the BIOS alone (shows the 3DO logo, then the "insert disc" screen)
./build/3do-emu ~/data/games/3DO/Panasonic-BIOS-PAL.bin

# Boot a game
./build/3do-emu ~/data/games/3DO/Panasonic-BIOS-PAL.bin "~/data/games/3DO/Doom (Europe).chd"
```

The emulator needs a real 3DO BIOS dump (1 MB), for example the Panasonic FZ-1 ROM. BIOS files are not included.

### Options

| Option | Effect |
|---|---|
| `--pal` / `--ntsc` | Forces the video standard. By default it is detected from the BIOS (`VIDEO SYSTEM :PAL` string). PAL outputs 384×288 at 50 Hz; NTSC outputs 320×240 at 59.94 Hz. |
| `--mmio-report` | Prints a report on exit of the unhandled or hottest MMIO registers, useful to find what the software is polling. |

### Drag and drop

You can drop files onto the window:

| Drop | Result |
|---|---|
| A BIOS image (1 MB or smaller, not a disc) | Loads it as the BIOS and resets |
| A disc of a different game | Inserts it and resets the console |
| Another disc of the same game (`(Disc 2)`, `(CD 2)`, `[CD 2]`…) | Hot-swaps it: the tray opens, then closes about 1 s later, with no reset |
| Any disc while holding **Ctrl** | Forces a hot swap |
| Any disc while holding **Shift** | Forces an insert and reset |

### Controls

| 3DO pad | Keyboard | Game controller |
|---|---|---|
| D-pad | Arrow keys | D-pad / left stick |
| A | `Z` | X (West) |
| B | `X` | A (South) |
| C | `C` | B (East) |
| L / R | `A` / `S` | Left / right shoulder |
| P (Play/Pause) | `Enter` | Start |
| X (Stop) | `Backspace` | Back / Select |

Other keys:

| Key | Action |
|---|---|
| `F5` | Reset the console |
| `Esc` | Quit |

Game controllers can be connected while the emulator is running.

### Disc inspection

```sh
./build/opera-ls <disc image> [directory path]
```

This prints the Opera FS volume label and the directory listing, for example to check that a disc contains `LaunchMe`.

### Diagnostics (environment variables)

| Variable | Traces |
|---|---|
| `EMU_TRACE_CD=1` | CD drive commands, status bytes and sector reads |
| `EMU_TRACE_DSP=1` | DSP register writes and FIFO activity (once per second) |
| `EMU_TRACE_CEL=1` | Every cel drawn: CCB flags, preambles, position, pixel counts |

The BIOS and games print debug text through MADAM's print port. It appears in the log prefixed with `[3DO]`.

---

## Development strategy

The project follows two guideline documents in [.claude/skills/](.claude/skills/):

- [emulator_dev.md](.claude/skills/emulator_dev.md) covers the engineering rules.
- [3do_specs.md](.claude/skills/3do_specs.md) covers the hardware reference.

The main principles:

1. **Incremental bring-up.** Build the infrastructure, then the memory map, the CPU and the peripherals. Each stage is testable before the next one starts.
2. **Real software drives the work.** Once the CPU ran, the real BIOS was the test case. Each blocker was diagnosed from what the BIOS polled, using the MMIO hotspot report, branch traces and watch PCs, then fixed. After that, each game's first failure picked the next feature to implement.
3. **No invented specs.** Behaviour comes from documented hardware facts or, where the documentation is silent, from the reference emulator Opera (the 4DO/FreeDO lineage). Code comments cite the source. Unknown registers are stubbed with rate-limited warnings (`Log::Limiter`), never guessed silently.
4. **Explicit endianness.** The 3DO is big-endian. All guest memory access goes through the helpers in [endian.hpp](src/common/endian.hpp), never through host-order casts.
5. **Headless core.** `emu_core` has no SDL dependency, so every component can be unit-tested and games can be run in scripted, headless harnesses. For example, a harness can press buttons at given frames and dump frames to PPM files to reproduce a bug.
6. **Modern C++.** The code uses C++20 with RAII, `std::span`, `std::format`, `constexpr` tables and fixed-width types from [types.hpp](src/common/types.hpp). It builds with no warnings under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`.

### Development steps

| # | Step | Result |
|---|---|---|
| 1 | Project structure, CMake, SDL2 loop | Window, a 60 Hz frame pacer, and the event, emulate and render loop |
| 2 | ARM60 CPU | ARMv3 interpreter |
| 3 | Memory map | DRAM, VRAM, ROM and the MMIO dispatch |
| 4 | Opera FS | Disc image reader, directory parser, `LaunchMe` lookup, `opera-ls` |
| 5 | Cel engine | Warped quad rasteriser, preambles, transparency, the pixel processor (PIXC) |
| 6 | Input | PBUS daisy chain, with SDL keyboard and controller mapping |
| 7–8 | Logging | Rate-limited warnings, MMIO register names, polling hotspot report |
| 9 | BIOS boot | CLIO, SPORT, timers, interrupts and the VDLP bring the PAL BIOS to the 3DO logo |
| 10 | XBUS / CD drive | MEI CD protocol, XBUS DMA and DIPIR: the BIOS boots a disc |
| 11 | Cel list and video | CCB list walking in MADAM; the VDLP renders VDL display lists |
| 12 | CHD discs | libchdr integration |
| 13 | DSP (DSPP) | DSP interpreter and audio DMA FIFOs; fixes Doom and Total Eclipse, adds sound |
| 14 | CPU timing | Opera's S/N/I cycle costs; fixes the Wing Commander III intro crash |
| 15 | Region detection | PAL/NTSC detected from the BIOS; the picture is centred |
| 16 | Cel precision | Half-pixel sampling rule; fixes glitchy FMV in WC3 |
| 17 | Drag and drop | Load a BIOS or disc, hot-swap discs of multi-CD games |
| 18 | LRFORM cels | Frame-buffer-format cels; fixes Wolfenstein 3D's 3D view |

---

## Architecture

```
            ┌───────────────────────── src/main.cpp (SDL2 frontend) ─────────────────────────┐
            │ window/texture · audio queue · SdlInput (keyboard/controller) · drag & drop    │
            └───────────────────────────────────────┬────────────────────────────────────────┘
                                                    │ run_frame() / framebuffer() / take_audio()
┌───────────────────────────────────── core::System (src/core/system.*) ─────────────────────────────────────┐
│  per-scanline scheduler: CPU slice → timers → VDLP line → VINT → DSP samples → XBUS/CD tick                │
│                                                                                                            │
│   Arm60 CPU ──► Memory / Bus ──┬── DRAM 2 MB + VRAM 1 MB ◄── SPORT (VRAM page copy/fill)                   │
│                                ├── ROM 1 MB (BIOS)                                                         │
│                                ├── MADAM ── cel engine (cel.cpp), VDL head, audio DMA FIFOs, PBUS DMA      │
│                                ├── CLIO ─── interrupts, timers, VCNT, DSP window, XBUS ── CD drive (MEI)   │
│                                └── SlowBus (NVRAM)                                  └── DiscImage/CHD      │
│   VDLP (video) · DSPP (audio DSP) · AudioDma · PlayerBus (pads)                                            │
└────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

### Source layout

```
src/
├── main.cpp                SDL2 frontend: arguments, main loop, rendering, audio, drag & drop
├── common/
│   ├── types.hpp           u8/u16/u32/s32… aliases
│   ├── endian.hpp          big-endian load/store helpers
│   └── log.hpp             Log::info/warn/error, warn_once, rate Limiter
├── frontend/
│   └── sdl_input.*         SDL keyboard/controller → 3DO pad state
└── core/
    ├── system.*            Owns all components, reset, scanline scheduler, disc insert/swap, region
    ├── bus/
    │   ├── memory.*        Memory map, ROM overlay at 0 until first write, MMIO dispatch
    │   ├── bus.hpp         Bus interface seen by the CPU (read/write/fetch)
    │   └── mmio_names.*    Register names for logs and the MMIO report
    ├── cpu/arm60.*         ARMv3 interpreter
    ├── madam/
    │   ├── madam.*         MADAM registers, cel list walker (SPRSTRT/NEXTCCB), DMA
    │   └── cel.*           Cel rasteriser: source decoders, projection, PIXC
    ├── clio/clio.*         CLIO: interrupts, 16 timers, VCNT, DMA enables, ExpCtl, DSP window
    ├── video/vdlp.*        VDL walker, CLUT, frame-buffer line pairs → ARGB scanlines
    ├── dsp/
    │   ├── dspp.*          DSPP audio DSP interpreter (runs once per 44.1 kHz sample)
    │   └── audio_dma.*     13 input and 4 output DMA FIFOs between RAM and the DSP
    ├── xbus/
    │   ├── xbus.*          Expansion bus: device selection, status/data FIFOs, DMA
    │   └── cd_drive.*      MEI CD-ROM drive: commands, status, 2× read timing, tray
    ├── cdrom/
    │   ├── disc_image.*    .iso/.bin (2048/2352-byte sectors) sector reader
    │   ├── chd_image.cpp   CHD reader through libchdr
    │   └── opera_fs.*      Opera file system parser
    ├── sport/sport.*       SPORT VRAM copy/flash-write
    ├── slowbus/slow_bus.*  SlowBus / NVRAM
    └── input/pbus.*        PBUS control pad protocol
tools/opera_ls.cpp          Disc listing tool
tests/                      Headless unit tests (test_common.hpp micro-framework)
```

### Memory map

| Address | Region |
|---|---|
| `0x00000000–0x001FFFFF` | DRAM, 2 MB (the ROM is overlaid here at reset until the first write) |
| `0x00200000–0x002FFFFF` | VRAM, 1 MB (contiguous with DRAM) |
| `0x03000000–0x030FFFFF` | BIOS ROM |
| `0x03100000` | SlowBus / NVRAM |
| `0x03200000` | SPORT |
| `0x03300000` | MADAM |
| `0x03400000` | CLIO (with the DSP window at `+0x1700–0x3FFF` and XBUS at `+0x500`) |

### Components

- **ARM60 CPU:** an ARMv3 interpreter with a 4096-entry decode table on instruction bits [27:20] and [7:4]. It implements banked registers for all modes, IRQ/FIQ, data aborts, and 26-bit-compatible PSR handling. Cycle costs follow Opera (S = 1, N = 2, I = 1). Wing Commander III needs this accurate timing.
- **Scheduler:** `System::run_frame()` runs one video field, scanline by scanline: 263 lines at 59.94 Hz for NTSC, 312 lines at 50 Hz for PAL, on a 12.5 MHz CPU. Each line runs a CPU slice, advances the CLIO timers, has the VDLP output the line, raises the vertical interrupts, generates DSP samples and ticks the CD drive.
- **MADAM and the cel engine:**
  - Writing SPRSTRT walks the CCB list. Each CCB has optional words (size, PIXC, PLUT), a preamble, and relative or absolute pointers.
  - Each source texel becomes a forward-mapped quad, split into two triangles with a top-left fill rule. Pixels are sampled at the pixel corner (`+0xFFFF` in 16.16), as in Opera.
  - Supported sources: 1/2/4/6/8/16 bpp, coded (PLUT) or uncoded, literal, packed (RLE) and LRFORM.
  - The PIXC pixel processor handles primary/secondary sources, scaling, add/subtract and averaging. It also supports transparency, the CECONTROL projector bits and the VRAM line-pair target layout.
- **VDLP:** follows the video display list from VDL head for each scanline. That covers control words, frame-buffer addresses, next pointers, CLUT/display/background words, and the 1-line "persist" counts. It converts line-pair frame-buffer pixels through the 3×32-entry CLUT to ARGB.
- **CLIO:** handles the interrupt banks (Int0 bit 31 is the Int1 summary), 16 cascaded timers, the VCNT beam counter, DMA enables and FIFO init, and ExpCtl. It also routes the DSP window: semaphore, reset/run, code memory, and input/output registers.
- **DSPP:** a port of Opera's DSP semantics. It runs the loaded program once per sample until `sleep`, reads and writes the audio DMA FIFOs, and outputs stereo 16-bit audio through `SDL_QueueAudio`.
- **XBUS and the CD drive:** implement the MEI command protocol (7-byte commands, status FIFO, data FIFO) and READ DATA at 150 sectors/s (2×). XBUS DMA into RAM raises Int0 bit 29. Tray open and close latch the "media changed" status bit for disc swaps.
- **Boot:** after power-on the BIOS runs its own code. The kernel performs DIPIR on the disc: it reads the Opera FS through the CD drive and launches `LaunchMe`. The emulator has no HLE boot.

### Reference material

- The **Opera** emulator (libretro `opera-libretro`, from FreeDO/4DO) is the behavioural reference where the hardware documentation is silent.
- The 3DO Portfolio SDK documentation covers cels, the CCB/preamble formats, VDLs and the DSP.

---

## Status and known limitations

- The core boot path and the games listed above run. Compatibility with other titles is untested.
- Not implemented:
  - NVRAM is not saved to a file between sessions;
  - there is no VDLP horizontal/vertical interpolation;
  - LRFORM cels are only supported at 16 bpp;
  - there are no save states.
- When an NTSC game runs on the PAL BIOS, a thin coloured line can appear near the bottom of the picture. This is authentic: the display list wraps around within the longer PAL field, in an area that a real TV hides as overscan.
