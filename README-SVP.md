# SVP (Sega Virtua Processor) support

This branch adds support for the **SVP**, the coprocessor Sega put inside the
*Virtua Racing* cartridge, to SGDK.

It follows the model SGDK already uses for the Z80: the coprocessor is
programmed in its own assembly language, in a `.svp` file assembled by an
external tool during the build, and driven from C on the 68000 side. There is
no C compiler for the SVP and this branch does not try to build one — the
SSP1601 has a six level hardware call stack that holds return addresses only,
no data stack, no frame pointer and 512 words of internal RAM. It is not a C
target.

## Contents

| path | what |
|------|------|
| `inc/svp/svp.h` | 68000 side C API and the `SVP_ROM_NOTES` header macro |
| `src/svp/svp.c` | its implementation, plus the reserved cartridge program area |
| `sample/svp/hello-svp/` | end to end sample: DSP writes DRAM and answers the mailbox, 68000 checks it and prints PASS or FAIL |
| `makefile.gen`, `common.mk`, `md.ld` | build integration (the only pre-existing files touched) |

## The hardware

The SVP is a Samsung SSP1601 DSP running at about 23 MHz inside the cartridge.

### Program space seen by the DSP (16 bit word addresses)

| range | contents |
|-------|----------|
| `0x0000`-`0x03FF` | IRAM, 1024 words. The DSP cannot write it with an ordinary store, only through the external memory path (PMC mode `0x001C`), which is how the boot code installs the interrupt handlers below. |
| `0x0400`-`0xFBFF` | external cartridge ROM (word `0x400` = cartridge byte `0x800`) |
| `0xFC00`-`0xFFFF` | ROM internal to the SVP chip |

### Boot

The DSP resets through the vector at `0xFFFC`, which points to `0xFC08` inside
the internal ROM. That boot code performs a TMSS style check against the Mega
Drive cartridge header and then jumps into cartridge code:

| header offset | meaning |
|---------------|---------|
| `0x1C8` | must contain the ASCII string `"SV"`. Without it the DSP jumps to an infinite loop at `0xFC00` and never runs. |
| `0x1CA` | flags word. Its low 10 bits are masked and must be 0 or 1. Bits 11 to 13 end up in the XST state register. |
| `0x1CC` | read and discarded. |
| `0x1CE` | the SVP entry point, as a word address in program space. *Virtua Racing* uses `0x0400`. The entry point is arbitrary. |

Before jumping, the boot code clears ST/X/Y/r0/r1/r2/r4/r5, writes default
interrupt handlers into IRAM at `0x3FA`/`0x3FC`/`0x3FE`, and sets `r6 = 0xFC`.
That `r6` value is a calling convention: the internal ROM's own library
routines do not use `call`/`ret`, they take the return address in `r6` and are
entered with `bra always`. New code should follow the same discipline.

The internal ROM is a callable library and not just a boot stub: data fill and
copy routines, 8.24 fixed point add/sub/multiply, a 256 word sine table at
`0xFEE3`, and rotation of a four vertex polygon around X, Y and Z. It costs no
cartridge ROM, so it is worth evaluating before writing equivalents. Note
however that no emulator listed below ships the internal ROM, so code calling
into it will only run on real hardware.

### Shared memory and the 68000 mailbox

| 68000 address | meaning |
|---------------|---------|
| `0x300000`-`0x31FFFF` | DRAM, 128 KB, read/write by both processors (mirrored up to `0x37FFFF`) |
| `0xA15000` / `0xA15002` | command/result register, seen as XST by the DSP. A 68000 write sets bit 1 of PM0. |
| `0xA15004` | status. A 68000 read clears bit 0. |
| `0xA15006` | halts the SVP. Real software writes `0x0A` before DMA'ing out of DRAM and `0` afterwards. |

DSP INT1 is wired to HBLANK.

## Talking to external memory from the DSP

With ST bits 5 and 6 clear the external registers are the 68000 interface:
`ext0` is the PM0 status register and `ext3` is the XST mailbox. With
`ST & 0x60` set they become external memory pointers instead.

An external access is set up by writing PMC (`ext6`) twice: first the 16 bit
word address, then the mode word. Mode `0x0018` selects DRAM with no auto
increment; DRAM word *N* is what the 68000 sees at `0x300000 + 2 * N`, so the
address word is simply *N*. A **blind** access to the pointer register
(`ld ext0, -`) must come between the PMC setup and the real access — it arms
the register, it does not move data.

`sample/svp/hello-svp/src/hello.svp` is a commented, working example of this.

## Installing `ssp16asm`

The build needs `ssp16asm`, the SSP16xx assembler from
[svpdev](https://github.com/jdesiloniz/svpdev) (MIT). It is not vendored here
and it is not required unless a project actually contains a `.svp` file.

```sh
git clone https://github.com/jdesiloniz/svpdev.git
cd svpdev/tools/ssp16asm
cargo build --release
cp target/release/ssp16asm ~/.local/bin/     # anywhere in PATH
```

The build looks it up in `PATH` and fails with an explicit message naming the
missing tool and where to get it. Override the name or path with `ASMSVP=` if
needed.

`ssp16asm` was used as found and needed no changes. Two of its quirks are worth
knowing when writing `.svp` code, and both are avoided in the sample:

* Every literal is hexadecimal, and the **digit count** decides the operand
  size: 1 to 2 digits make a byte, 3 to 4 digits make a word. `org`, `equ` and
  word immediates therefore have to be written padded (`org 400`, not `org 40`).
* `dw` with a 1 to 2 digit value emits a word but only advances the first pass
  symbol table by zero, so every label after such a line gets a wrong address.
  Always give `dw` a 4 digit value.

## Build integration

A `.svp` file anywhere under `src/` is picked up automatically, exactly like a
`.s80` file:

1. `md.ld` pins a `.svp_program` section at cartridge byte `0x800` whenever the
   symbol `SVP_program` is linked in, reserving `SVP_PROGRAM_SIZE` bytes
   (default `0x800`) so that no 68000 code lands in the DSP's entry area. A
   project that does not use the SVP is not affected at all.
2. After link, `injectSVP` runs `ssp16asm -b out/release/rom.bin` over each
   `.svp` file, writing the DSP opcodes into the ROM image at
   `org_word * 2`. This is a post-link step, so `md.ld` needed only the
   reservation and no relocation logic.
3. `injectSVP` runs **before** `padROM`, so the Mega Drive ROM checksum covers
   the injected code.
4. The step reads the real bounds of the reserved area back out of the linked
   ELF with `nm -S` and compares the ROM image before and after injection, so a
   `.svp` file that overruns the area, or one whose `org` is below `400`, fails
   the build instead of silently overwriting 68000 code. The link also forces
   `SVP_program` in with `-Wl,-u`, so the reservation happens even for a project
   that has a `.svp` file but never calls the C API.

To give the DSP more room, rebuild the library with a bigger area. The size
lives in one place only, so nothing has to be kept in sync:

```sh
make -f makelib.gen EXTRA_FLAGS=-DSVP_PROGRAM_SIZE=0x2000
```

## Declaring the header

The `notes` field of `ROMHeader` starts at ROM offset `0x1C8`, which is exactly
where the SVP boot parameters live. The entry point and flags are binary words
and not printable text, so the header is declared with a macro that expands to
a braced initializer rather than with a string literal. In your own
`src/rom_header.c`:

```c
#include <genesis.h>
#include <svp/svp.h>

__attribute__((externally_visible))
const ROMHeader rom_header = {
    "SEGA MEGA DRIVE ",
    /* ... */
    "            ",
    SVP_ROM_NOTES(0, SVP_ENTRY_POINT),
    "JUE             "
};
```

Without it the DSP never leaves its internal ROM.

## Building and running the sample

```sh
# once, from the SGDK folder
make -f makelib.gen

# then
cd sample/svp/hello-svp
make -f ../../../makefile.gen
```

`out/rom.bin` is a 128 KB ROM. The 68000 sends command `0x1234` through the
mailbox; the DSP writes `0xC0DE` into DRAM word 0 and answers with the
complement of the command; the 68000 checks both and prints `PASS` or `FAIL`.
There is no 3D and no rendering, only the plumbing.

## Emulators

**Genesis Plus GX is the only emulator that detects the SVP the way the
hardware does**, by checking for `"SV"` at `0x1C8` (`md_cart_init()` in
`core/cart_hw/md_cart.c`). ares, MAME and PicoDrive instead check whether the
cartridge title is `Virtua Racing`, so a correct homebrew SVP ROM will simply
run without a DSP on them. That is not a bug in your ROM.

```sh
retroarch -L /usr/lib/libretro/genesis_plus_gx_libretro.so out/rom.bin
```

The core path is distribution dependent; a RetroArch that downloaded the core
itself keeps it in `~/.config/retroarch/cores/genesis_plus_gx_libretro.so`.

Two things Genesis Plus GX does that differ from the contract above, worth
knowing before you debug a phantom problem:

* **`0xA15006` is not implemented.** Only `0xA15000`, `0xA15002` and `0xA15004`
  are decoded (`ctrl_io_write_word` / `ctrl_io_read_word`, `case 0x50`, in
  `core/mem68k.c`); everything else in that block is discarded on write and
  reads back as open bus. The halt guard in this API is therefore a no-op under
  Genesis Plus GX. It is still correct to use it — the emulator runs the 68000
  and the DSP serially per scanline, so there is no bus contention to guard
  against there, but real hardware has it.
* **The header entry point is ignored.** `ssp1601_reset()` unconditionally sets
  the DSP program counter to word `0x400`, so the emulator never runs the
  internal boot ROM and never reads offset `0x1CE`. Keep the entry point at
  `SVP_ENTRY_POINT` (`0x400`) if you want the same code to run on both the
  emulator and real hardware.

The internal SVP ROM at `0xFC00` is not present in Genesis Plus GX either, so
its library routines and the sine table are unavailable under emulation.
