# SGDK-PC — a native PC backend for SGDK games (experimental)

This directory adds an **API layer**: it reimplements part of the SGDK API
on top of SDL2, so the same C sources that build a Mega Drive ROM can also
be compiled for x86 and run natively — no emulator, and therefore none of
the console's limits (61 on-screen colours, 80 sprites, 64 KB of VRAM,
the 68000's cycle budget).

It is **not** a bundled emulator. Tools that advertise "SGDK compiles to
.exe" usually ship the ROM inside an emulator runtime; the game still
executes as 68000 code and every hardware limit stays. Here the game code
is compiled for the host.

## Status: proof of concept

Extracted from a working game (a Mega Drive port of *Pitfall!*, which runs
natively at 1280x896 and 60 fps from a single 3 MB executable). It
currently implements the **15 functions that game used**:

```
VDP_setScreenWidth320   VDP_setBackgroundColor   VDP_loadTileData
VDP_setTileMapDataRow   VDP_fillTileMapRect      VDP_setSpriteFull
VDP_updateSprites       VDP_setScrollingMode     VDP_setHorizontalScrollTile
PAL_setColors           JOY_readJoypad           SYS_doVBlankProcess
PSG_setTone             PSG_setEnvelope          PSG_setNoise
```

plus the types and macros around them (`TILE_ATTR_FULL`, `SPRITE_SIZE`,
`PAL0..3`, `BG_A/BG_B`, the `BUTTON_*` bits, `GET_VCOUNTER`).

**Not implemented yet**: the `SPR_*` sprite engine, `XGM`/Z80 music
(the driver is Z80 code — the YM2612 would need a chip core), text/font
helpers, window plane, shadow/highlight, DMA queue semantics beyond what
the sample game needed, and anything that writes VDP ports directly or
uses raster effects on the horizontal interrupt.

So: a game that sticks to the API subset above will build and run. Most
real games will hit a missing function on the first try — the point of
publishing this is that adding one is usually small, and the hard part
(the VDP model) is already here.

## What it models

- VRAM/CRAM as arrays; 4bpp tiles, 8 `u32` per tile, high nibble is the
  leftmost pixel; 9-bit BGR colour words.
- Two planes of 64x32 cells (priority / palette / flip / tile index).
- Composition order, back to front: backdrop, low-priority plane B,
  low-priority plane A, low-priority sprites, then the same three at high
  priority. Games rely on this (hiding a sprite behind high-priority
  background cells is a common trick).
- Sprites with column-major tiles and the hardware link list.
- Per-tile-row horizontal scroll.
- Audio: a software **SN76489** (`psg_pc.c`), validated against the
  frequency formula to within 0.1 %. Chip emulation, not CPU emulation.
- Input: keyboard plus SDL GameController (XInput on Windows) — D-pad and
  left stick both move, any face/shoulder button is a fire button.

## Using it

Your game keeps including `genesis.h`. Add a build-time branch in your own
header, as the sample game does:

```c
#ifdef PC_BUILD
#include "pc/sgdk_pc.h"
#else
#include <genesis.h>
#endif
```

SGDK games define `int main(bool hardReset)`; under `PC_BUILD` the header
renames it and `pc/src/main_pc.c` provides the real entry point.

Build with clang or gcc, `-DPC_BUILD`, linking SDL2. `pc/build_sdl2_static.ps1`
builds a static SDL2 on Windows so the result is a single executable with
no DLL beside it.

Useful switches implemented in the backend: `--scale 3|4`, `--frames N`,
`--screenshot file.bmp` (deterministic capture; combine with
`SDL_VIDEODRIVER=dummy` for a completely headless run), `--input-debug`.

## Licence

SGDK is MIT (Stephane Dallongeville). This directory is contributed under
the same terms.
