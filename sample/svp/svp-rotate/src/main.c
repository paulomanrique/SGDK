#include <genesis.h>
#include <svp/svp.h>

// protocol, kept in sync with src/rotate.svp
#define SVP_CMD_FRAME       0x0001      // "rotate the points once"
#define SVP_REPLY_DONE      0x0055      // "the coordinates are in DRAM"

#define POINT_COUNT         8
#define COORD_WORDS         (POINT_COUNT * 2)

#define SPRITE_TILE         TILE_USER_INDEX
#define SVP_TIMEOUT         0xFFFF


// a 16x16 shaded blob, built at runtime so the sample needs no resource file
static void loadBlobTiles(void)
{
    u32 tiles[4][8];
    u16 t, r, c;

    for(t = 0; t < 4; t++)
        for(r = 0; r < 8; r++) tiles[t][r] = 0;

    for(r = 0; r < 16; r++)
    {
        for(c = 0; c < 16; c++)
        {
            // distance from the centre of the 16x16 square, times two
            const s16 dx = (c * 2) - 15;
            const s16 dy = (r * 2) - 15;
            const s16 d2 = (dx * dx) + (dy * dy);

            u16 color;

            if (d2 > 225) color = 0;            // outside the blob
            else if (d2 > 121) color = 2;
            else if (d2 > 36) color = 3;
            else color = 4;

            if (color)
            {
                // a 2x2 sprite stores its tiles column by column
                const u16 tile = ((c >> 3) * 2) + (r >> 3);
                const u16 shift = (7 - (c & 7)) * 4;

                tiles[tile][r & 7] |= ((u32) color) << shift;
            }
        }
    }

    VDP_loadTileData((const u32*) tiles, SPRITE_TILE, 4, CPU);
}


int main(bool hardReset)
{
    u16 coords[COORD_WORDS];
    u16 i;

    VDP_setScreenWidth320();
    VDP_setBackgroundColor(0);

    if (!SVP_isPresent())
    {
        VDP_drawText("No SVP marker in the ROM header.", 2, 12);
        VDP_drawText("This demo needs Genesis Plus GX.", 2, 14);

        while(TRUE) SYS_doVBlankProcess();
    }

    PAL_setColor(2, RGB24_TO_VDPCOLOR(0x2040A0));
    PAL_setColor(3, RGB24_TO_VDPCOLOR(0x40A0E0));
    PAL_setColor(4, RGB24_TO_VDPCOLOR(0xC0F0FF));

    loadBlobTiles();

    VDP_drawText("SVP rotates, 68000 only places", 4, 1);

    SVP_reset();

    while(TRUE)
    {
        u16 reply;

        if (!SVP_waitReply(SVP_CMD_FRAME, &reply, SVP_TIMEOUT) || (reply != SVP_REPLY_DONE))
        {
            VDP_drawText("The SVP stopped answering.", 4, 12);

            while(TRUE) SYS_doVBlankProcess();
        }

        // read the finished screen coordinates back. This one does halt the
        // DSP around the access, which is what SVP_readDRAM is for.
        SVP_readDRAM(0, coords, COORD_WORDS);

        for(i = 0; i < POINT_COUNT; i++)
        {
            VDP_setSpriteFull(i, (s16) coords[i * 2], (s16) coords[(i * 2) + 1],
                              SPRITE_SIZE(2, 2),
                              TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, SPRITE_TILE),
                              (i + 1) % POINT_COUNT);
        }

        VDP_updateSprites(POINT_COUNT, DMA_QUEUE);

        SYS_doVBlankProcess();
    }

    return 0;
}
