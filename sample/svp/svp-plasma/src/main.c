#include <genesis.h>
#include <svp/svp.h>

// protocol, kept in sync with src/plasma.svp
#define SVP_CMD_FRAME       0x0001      // "compute one plasma frame into DRAM"
#define SVP_REPLY_DONE      0x0055      // what the DSP answers when the frame is ready

// The DSP writes ready made tilemap words, so it needs to know where the solid
// colour tiles live. Keep TILE_BASE identical to TileBase in plasma.svp.
#define TILE_BASE           TILE_USER_INDEX
#define PLASMA_COLORS       16

// plane A is 64 tiles wide, so 28 rows of 64 words cover the whole visible
// screen as one contiguous block and the frame is a single DMA
#define GRID_W              64
#define GRID_H              28
#define GRID_SIZE           (GRID_W * GRID_H)

#define SVP_TIMEOUT         0xFFFF


// one solid colour tile per palette entry, so a tilemap word is a pixel
static void loadColorTiles(void)
{
    u32 tile[8];
    u16 i, j;

    for(i = 0; i < PLASMA_COLORS; i++)
    {
        // Every pixel of the tile is palette index i. The UL matters: without
        // it the last iteration computes 15 * 0x11111111 in a signed int,
        // which overflows, and -O3 is entitled to make a mess of the loop.
        const u32 row = (u32) i * 0x11111111UL;

        for(j = 0; j < 8; j++) tile[j] = row;

        VDP_loadTileData(tile, TILE_BASE + i, 1, CPU);
    }
}

static void setPlasmaPalette(void)
{
    // A plasma wraps: index 15 sits right next to index 0, so the ramp has to
    // close on itself or a hard seam appears wherever the field rolls over.
    static const u16 wheel[16] =
    {
        0x0E00, 0x0E04, 0x0C08, 0x0A0C, 0x080E, 0x040E, 0x000E, 0x020C,
        0x040A, 0x0608, 0x0806, 0x0A04, 0x0C02, 0x0E02, 0x0E00, 0x0E00
    };

    PAL_setColors(0, wheel, 16, CPU);
}


#if (TILE_BASE < TILE_USER_INDEX)
#error "TILE_BASE overlaps the tiles SGDK reserves for itself"
#endif


int main(bool hardReset)
{
    VDP_setScreenWidth320();
    VDP_setBackgroundColor(0);

    if (!SVP_isPresent())
    {
        VDP_drawText("No SVP marker in the ROM header.", 2, 12);
        VDP_drawText("This demo needs Genesis Plus GX.", 2, 14);

        while(TRUE) SYS_doVBlankProcess();
    }

    loadColorTiles();
    setPlasmaPalette();

    SVP_reset();

    while(TRUE)
    {
        u16 reply;

        // Ask the DSP for a frame and wait until it says DRAM is ready. The
        // DSP is idle between its reply and the next command, so nothing can
        // touch DRAM while the DMA below reads it and no halt guard is needed.
        if (!SVP_waitReply(SVP_CMD_FRAME, &reply, SVP_TIMEOUT) || (reply != SVP_REPLY_DONE))
        {
            VDP_drawText("The SVP stopped answering.", 2, 12);

            while(TRUE) SYS_doVBlankProcess();
        }

        // The DSP produced finished tilemap words, so the 68000 only moves
        // them: one DMA straight out of cartridge DRAM into video memory.
        DMA_transfer(DMA_QUEUE, DMA_VRAM, (void*) SVP_DRAM, VDP_BG_A, GRID_SIZE, 2);

        SYS_doVBlankProcess();
    }

    return 0;
}
