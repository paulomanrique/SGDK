#ifndef SGDK_PC_H
#define SGDK_PC_H

#include <stdint.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef u8       bool;

#define FALSE 0
#define TRUE  1

typedef enum {
    CPU = 0,
    DMA = 1,
    DMA_QUEUE = 2,
    DMA_QUEUE_COPY = 3
} TransferMethod;

typedef enum {
    BG_A = 0,
    BG_B = 1,
    WINDOW = 2
} VDPPlane;

#define TILE_ATTR_PRIORITY_MASK 0x8000u
#define TILE_ATTR_FULL(pal, prio, flipV, flipH, index) \
    ((u16)((((u16)(prio) & 1u) << 15) | (((u16)(pal) & 3u) << 13) | \
           (((u16)(flipV) & 1u) << 12) | (((u16)(flipH) & 1u) << 11) | \
           ((u16)(index) & 0x07FFu)))
#define SPRITE_SIZE(w, h) ((u8)((((u8)(w) - 1u) << 2) | ((u8)(h) - 1u)))

#define TILE_USER_INDEX 16

#define PAL0 0
#define PAL1 1
#define PAL2 2
#define PAL3 3

#define HSCROLL_TILE  2
#define VSCROLL_PLANE 0

#define JOY_1        0x0000
#define BUTTON_UP    0x0001
#define BUTTON_DOWN  0x0002
#define BUTTON_LEFT  0x0004
#define BUTTON_RIGHT 0x0008
#define BUTTON_B     0x0010
#define BUTTON_C     0x0020
#define BUTTON_A     0x0040
#define BUTTON_START 0x0080

#define PSG_ENVELOPE_MIN       15
#define PSG_NOISE_TYPE_PERIODIC 0
#define PSG_NOISE_TYPE_WHITE    1
#define PSG_NOISE_FREQ_CLOCK2   0
#define PSG_NOISE_FREQ_CLOCK4   1
#define PSG_NOISE_FREQ_CLOCK8   2
#define PSG_NOISE_FREQ_TONE3    3

void VDP_setScreenWidth320(void);
void VDP_setBackgroundColor(u8 index);
void VDP_loadTileData(const u32 *data, u16 index, u16 num, TransferMethod tm);
void VDP_setTileMapDataRow(VDPPlane plane, const u16 *row, u16 rowIdx,
                           u16 x, u16 len, TransferMethod tm);
void VDP_fillTileMapRect(VDPPlane plane, u16 attr, u16 x, u16 y, u16 w, u16 h);
void VDP_setSpriteFull(u16 index, s16 x, s16 y, u8 size, u16 attr, u16 link);
void VDP_updateSprites(u16 num, TransferMethod tm);
void VDP_setScrollingMode(u16 h, u16 v);
void VDP_setHorizontalScrollTile(VDPPlane plane, u16 tile, s16 *values,
                                 u16 len, TransferMethod tm);
void PAL_setColors(u16 index, const u16 *pal, u16 count, TransferMethod tm);
u16 JOY_readJoypad(u16 joy);
void SYS_doVBlankProcess(void);
/* Thread-safe PSG entry points (lock around the psg_pc core; audio callback
 * holds the same lock while rendering). Game code keeps calling PSG_set*. */
void SGDK_PC_PSG_setTone(u8 channel, u16 value);
void SGDK_PC_PSG_setEnvelope(u8 channel, u8 value);
void SGDK_PC_PSG_setNoise(u8 type, u8 frequency);
#define PSG_setTone(c, v)     SGDK_PC_PSG_setTone((c), (v))
#define PSG_setEnvelope(c, v) SGDK_PC_PSG_setEnvelope((c), (v))
#define PSG_setNoise(t, f)    SGDK_PC_PSG_setNoise((t), (f))

u16 SGDK_PC_getVCounter(void);
#define GET_VCOUNTER SGDK_PC_getVCounter()

int SGDK_PC_init(int argc, char **argv);
int SGDK_PC_is_md(void);
void SGDK_PC_set_fullcolor_frame(const u32 *pixels, u16 width, u16 height);

#ifdef PC_BUILD
#define main sgdk_game_main
#endif

#endif
