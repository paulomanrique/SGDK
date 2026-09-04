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
| `sample/svp/svp-plasma/` | demo: the DSP renders an animated plasma as finished tilemap words, the 68000 only DMAs it |
| `sample/svp/svp-rotate/` | demo: the DSP rotates points with its hardware multiplier, the 68000 only places sprites |
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
* **`ld a, A[nn]` and `ld a, B[nn]` are miscounted the same way**, and this one
  is not documented upstream. The instruction emits one word but the symbol
  pass credits it with zero, so every label defined after it is one word too
  low and every branch to such a label jumps one instruction early. The emitted
  opcodes are correct; only the symbol table is wrong, which makes the failure
  silent and very hard to read. Minimal reproduction:

  ```
  org 400
      ld a, B[00]
      ld a, x
  L:  ld a, x
  ```

  `L` is reported at `0401` where the first two instructions plainly occupy two
  words, so it should be `0402`. One word is lost per occurrence. Both demos
  therefore avoid the RAM bank direct load entirely and reach internal RAM
  through the pointer registers (`ld a, (r0)`, `ld (r4), a`), which assemble
  and count correctly. The store direction, `ld B[nn], a`, is also fine.

  This cost real debugging time here: the first plasma build hung because
  `rowLoop` resolved one word early, onto the `ldi r5, 1C` that reloads the row
  counter, so the loop reset its own counter forever.

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
make -f makelib.gen clean
make -f makelib.gen EXTRA_FLAGS=-DSVP_PROGRAM_SIZE=0x2000
```

The `clean` is required and not decoration: changing a variable on the make
command line does not make the existing `svp.o` out of date, so without it the
library is silently relinked with the old size and the reservation does not
grow. Check the result with
`m68k-elf-nm -S out/release/rom.out | grep SVP_program`, whose second column is
the size actually reserved.

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

## The two demos

Both follow the same division of labour the SVP was built for: the DSP does the
arithmetic and leaves a finished result in DRAM, and the 68000 does nothing but
move bytes.

### svp-plasma

The DSP renders a full screen animated plasma, 64 by 28 cells, straight into
DRAM as ready made VDP tilemap words. The 68000 never computes a pixel: it
waits for the "frame ready" answer and issues a single DMA from cartridge DRAM
into video memory. Plane A is 64 tiles wide, so 28 rows of 64 words are one
contiguous block and one transfer.

The sine table is copied into internal RAM bank A at boot. Bank A is exactly
256 words and the pointer registers are 8 bit, so `ld a, (r0+)` walks the table
and wraps around by itself: the inner loop needs no masking at all, which is
what makes nine instructions per cell enough.

### svp-rotate

The DSP rotates eight points around the screen centre every frame and writes
the finished screen coordinates into DRAM; the 68000 reads them back with
`SVP_readDRAM()` (which brackets the access with the halt guard) and drops
sprites on them.

This one exercises the multiplier. It is a signed Q15 fractional unit: with X
an integer and Y a sine scaled to +-32767, `ld a, p` leaves `X * (Y/32768)` in
the high word of the accumulator, which is exactly `x*cos` with no shifting.
`add a, p` and `sub a, p` then fold in the second term, so a full
`x*cos - y*sin` needs six instructions and no temporary storage:

```
ld x, <x>
ld y, (r4)      # cos, parked in RAM bank B
ld a, p         # A = x * cos
ld x, <y>
ld y, (r4)      # sin
sub a, p        # A = x*cos - y*sin
```

## Emulators

**Genesis Plus GX is the only emulator that detects the SVP the way the
hardware does**, by checking for `"SV"` at `0x1C8` (`md_cart_init()` in
`core/cart_hw/md_cart.c`). ares, MAME, PicoDrive and Kega Fusion instead look
at the cartridge title, so a correct homebrew SVP ROM runs without a DSP on
them unless it claims to be Virtua Racing.

The samples here satisfy both. Their overseas name at ROM offset `0x150` starts
with `VIRTUA RACING` and then names the demo, for example
`VIRTUA RACING SVP PLASMA`. That works because the title check is a prefix
compare: PicoDrive's `carthw.cfg` carries `check_str=0x150,"VIRTUA RACING"` and
`rom_strcmp()` only compares `strlen()` characters, so the tail of the field is
free. Genesis Plus GX ignores the title entirely and still keys off the `"SV"`
marker, so nothing regresses there.

If you write your own SVP project and it runs on Genesis Plus GX but shows
nothing anywhere else, this title is the first thing to check.

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
