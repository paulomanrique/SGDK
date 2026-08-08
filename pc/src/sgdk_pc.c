#ifdef PC_BUILD
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "sgdk_pc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* psg_pc core (same symbols as the SGDK-named API). Undef the header macros
 * so these declarations refer to the real implementations in psg_pc.c. */
#undef PSG_setTone
#undef PSG_setEnvelope
#undef PSG_setNoise
void PSG_setTone(u8 channel, u16 value);
void PSG_setEnvelope(u8 channel, u8 value);
void PSG_setNoise(u8 type, u8 frequency);
void psg_pc_init(void);
void psg_pc_render(s16 *buffer, int frames);
void psg_pc_shutdown(void);

#define MD_SCREEN_W 320
#define MD_SCREEN_H 224
#define PC_SCREEN_W 1280
#define PC_SCREEN_H 896
#define PLANE_W 64
#define PLANE_H 32
#define TILE_COUNT 2048
#define MAX_SPRITES 80

/* SDL audio: mono s16 @ 44.1 kHz, ~23 ms buffer (1024 frames). */
#define AUDIO_RATE        44100
#define AUDIO_BUFFER_FRAMES 1024

typedef struct {
    s16 x;
    s16 y;
    u8 size;
    u16 attr;
    u16 link;
} PcSprite;

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static u32 pixels[MD_SCREEN_W * MD_SCREEN_H];
static const u32 *fullcolorPixels;
static int textureW = MD_SCREEN_W;
static int textureH = MD_SCREEN_H;

static u32 vram[TILE_COUNT][8];
static u16 cram[64];
static u16 planes[2][PLANE_H][PLANE_W];
static s16 hscroll[2][PLANE_H];
static PcSprite spriteWork[MAX_SPRITES];
static PcSprite sprites[MAX_SPRITES];
static u16 spriteCount;
static u8 backdropIndex;

static int running = 1;
static int mdMode;
static int frameLimit;
static int frameNumber;
static const char *screenshotPath;
static const char *dumpWavPath;
static int verifyAudio;
static int inputDebug;
static uint64_t nextFrame;
static uint64_t perfFrequency;
static uint64_t frameRemainder;
static uint64_t statsStart;
static uint64_t lastInputDebugTick;

/* First-opened SDL GameController; additional pads are ignored. */
static SDL_GameController *gameController;
static SDL_JoystickID gameControllerInstance = -1;

/* Digital threshold for left stick (full 32767 range). Resting sticks sit
 * well under this; anything past it is treated as fully pressed. */
#define STICK_DEADZONE 8000
/* Triggers are analog axes; half travel (~16384) counts as a button. */
#define TRIGGER_THRESHOLD 16384

/* game.c globals — used only by --verify-audio to cue real sound sequences */
extern u8 soundIdx;
extern u8 soundDelay;

static SDL_AudioDeviceID audioDevice;
static SDL_mutex *psgMutex;

/* Optional WAV capture of the mixed stream (for offline verification). */
static FILE *dumpWavFile;
static u32 dumpWavFrames;
static u32 dumpWavDataBytes;

static u32 color_from_cram(u16 color)
{
    u8 r = (u8)((color >> 1) & 7u);
    u8 g = (u8)((color >> 5) & 7u);
    u8 b = (u8)((color >> 9) & 7u);
    r = (u8)((r * 255u + 3u) / 7u);
    g = (u8)((g * 255u + 3u) / 7u);
    b = (u8)((b * 255u + 3u) / 7u);
    return 0xFF000000u | ((u32)r << 16) | ((u32)g << 8) | b;
}

static u8 tile_pixel(u16 attr, u8 x, u8 y)
{
    u16 tile = attr & 0x07FFu;
    if (attr & 0x0800u)
        x = (u8)(7u - x);
    if (attr & 0x1000u)
        y = (u8)(7u - y);
    return (u8)((vram[tile][y] >> (28u - (u32)x * 4u)) & 0x0Fu);
}

static void draw_plane(VDPPlane plane, int highPriority)
{
    for (int y = 0; y < MD_SCREEN_H; y++) {
        int sourceXBase = -hscroll[plane][y >> 3];
        for (int x = 0; x < MD_SCREEN_W; x++) {
            int sourceX = (x + sourceXBase) & (PLANE_W * 8 - 1);
            u16 attr = planes[plane][y >> 3][sourceX >> 3];
            if (((attr & TILE_ATTR_PRIORITY_MASK) != 0) != highPriority)
                continue;
            u8 color = tile_pixel(attr, (u8)(sourceX & 7), (u8)(y & 7));
            if (color != 0) {
                u8 palette = (u8)((attr >> 13) & 3u);
                pixels[y * MD_SCREEN_W + x] = color_from_cram(cram[palette * 16u + color]);
            }
        }
    }
}

static int sprite_order(u16 *order)
{
    if (spriteCount == 0)
        return 0;

    u8 seen[MAX_SPRITES] = {0};
    u16 index = 0;
    int count = 0;
    while (index < spriteCount && index < MAX_SPRITES && !seen[index]) {
        seen[index] = 1;
        order[count++] = index;
        if (sprites[index].link == 0)
            break;
        index = sprites[index].link;
    }
    return count;
}

static void draw_sprites(int highPriority)
{
    u16 order[MAX_SPRITES];
    int count = sprite_order(order);

    /* Earlier entries in the linked SAT have priority over later entries. */
    for (int oi = count - 1; oi >= 0; oi--) {
        PcSprite *sprite = &sprites[order[oi]];
        if (((sprite->attr & TILE_ATTR_PRIORITY_MASK) != 0) != highPriority)
            continue;

        int widthCells = ((sprite->size >> 2) & 3) + 1;
        int heightCells = (sprite->size & 3) + 1;
        int width = widthCells * 8;
        int height = heightCells * 8;
        int x0 = sprite->x < 0 ? 0 : sprite->x;
        int y0 = sprite->y < 0 ? 0 : sprite->y;
        int x1 = sprite->x + width > MD_SCREEN_W ? MD_SCREEN_W : sprite->x + width;
        int y1 = sprite->y + height > MD_SCREEN_H ? MD_SCREEN_H : sprite->y + height;
        u16 baseTile = sprite->attr & 0x07FFu;
        u8 palette = (u8)((sprite->attr >> 13) & 3u);

        for (int y = y0; y < y1; y++) {
            int sy = y - sprite->y;
            if (sprite->attr & 0x1000u)
                sy = height - 1 - sy;
            for (int x = x0; x < x1; x++) {
                int sx = x - sprite->x;
                if (sprite->attr & 0x0800u)
                    sx = width - 1 - sx;
                u16 tile = (u16)(baseTile + (sx >> 3) * heightCells + (sy >> 3));
                if (tile >= TILE_COUNT)
                    continue;
                u8 color = (u8)((vram[tile][sy & 7] >>
                                 (28u - (u32)(sx & 7) * 4u)) & 0x0Fu);
                if (color != 0)
                    pixels[y * MD_SCREEN_W + x] = color_from_cram(cram[palette * 16u + color]);
            }
        }
    }
}

static void compose_frame(void)
{
    u32 backdrop = color_from_cram(cram[backdropIndex & 63u]);
    for (int i = 0; i < MD_SCREEN_W * MD_SCREEN_H; i++)
        pixels[i] = backdrop;

    draw_plane(BG_B, 0);
    draw_plane(BG_A, 0);
    draw_sprites(0);
    draw_plane(BG_B, 1);
    draw_plane(BG_A, 1);
    draw_sprites(1);
}

static void write_u16_le(FILE *f, u16 v)
{
    u8 b[2] = {(u8)(v & 0xffu), (u8)(v >> 8)};
    fwrite(b, 1, 2, f);
}

static void write_u32_le(FILE *f, u32 v)
{
    u8 b[4] = {(u8)(v & 0xffu), (u8)((v >> 8) & 0xffu),
               (u8)((v >> 16) & 0xffu), (u8)(v >> 24)};
    fwrite(b, 1, 4, f);
}

static int open_dump_wav(const char *path)
{
    dumpWavFile = fopen(path, "wb");
    if (!dumpWavFile) {
        fprintf(stderr, "Could not open WAV dump '%s'\n", path);
        return 0;
    }
    /* Placeholder header; sizes patched in close_dump_wav. */
    fwrite("RIFF", 1, 4, dumpWavFile);
    write_u32_le(dumpWavFile, 0);
    fwrite("WAVE", 1, 4, dumpWavFile);
    fwrite("fmt ", 1, 4, dumpWavFile);
    write_u32_le(dumpWavFile, 16);                 /* PCM chunk size */
    write_u16_le(dumpWavFile, 1);                  /* PCM */
    write_u16_le(dumpWavFile, 1);                  /* mono */
    write_u32_le(dumpWavFile, (u32)AUDIO_RATE);
    write_u32_le(dumpWavFile, (u32)AUDIO_RATE * 2u); /* byte rate */
    write_u16_le(dumpWavFile, 2);                  /* block align */
    write_u16_le(dumpWavFile, 16);                 /* bits */
    fwrite("data", 1, 4, dumpWavFile);
    write_u32_le(dumpWavFile, 0);
    dumpWavFrames = 0;
    dumpWavDataBytes = 0;
    return 1;
}

static void close_dump_wav(void)
{
    if (!dumpWavFile)
        return;
    u32 dataBytes = dumpWavDataBytes;
    u32 riffSize = 36u + dataBytes;
    fseek(dumpWavFile, 4, SEEK_SET);
    write_u32_le(dumpWavFile, riffSize);
    fseek(dumpWavFile, 40, SEEK_SET);
    write_u32_le(dumpWavFile, dataBytes);
    fclose(dumpWavFile);
    dumpWavFile = NULL;
    fprintf(stderr, "WAV dump: %u frames (%.2f s) -> %s\n",
            (unsigned)dumpWavFrames,
            (double)dumpWavFrames / (double)AUDIO_RATE,
            dumpWavPath ? dumpWavPath : "(unknown)");
}

static void SDLCALL psg_audio_callback(void *userdata, Uint8 *stream, int len)
{
    int frames = len / (int)sizeof(s16);
    (void)userdata;
    if (frames <= 0) {
        memset(stream, 0, (size_t)len);
        return;
    }
    if (psgMutex)
        SDL_LockMutex(psgMutex);
    psg_pc_render((s16 *)stream, frames);
    if (psgMutex)
        SDL_UnlockMutex(psgMutex);

    if (dumpWavFile) {
        fwrite(stream, 1, (size_t)len, dumpWavFile);
        dumpWavFrames += (u32)frames;
        dumpWavDataBytes += (u32)len;
    }
}

static int open_audio(void)
{
    SDL_AudioSpec want, have;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL audio subsystem failed: %s\n", SDL_GetError());
        return 0;
    }

    psgMutex = SDL_CreateMutex();
    if (!psgMutex) {
        fprintf(stderr, "SDL_CreateMutex failed: %s\n", SDL_GetError());
        return 0;
    }

    psg_pc_init();

    SDL_zero(want);
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = AUDIO_BUFFER_FRAMES;
    want.callback = psg_audio_callback;
    want.userdata = NULL;

    audioDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!audioDevice) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return 0;
    }
    if (have.freq != AUDIO_RATE || have.format != AUDIO_S16SYS ||
        have.channels != 1) {
        fprintf(stderr,
                "SDL audio format mismatch (got %d Hz, fmt=%d, ch=%d)\n",
                have.freq, (int)have.format, (int)have.channels);
        SDL_CloseAudioDevice(audioDevice);
        audioDevice = 0;
        return 0;
    }

    SDL_PauseAudioDevice(audioDevice, 0);
    return 1;
}

static void close_audio(void)
{
    if (audioDevice) {
        SDL_PauseAudioDevice(audioDevice, 1);
        SDL_CloseAudioDevice(audioDevice);
        audioDevice = 0;
    }
    close_dump_wav();
    psg_pc_shutdown();
    if (psgMutex) {
        SDL_DestroyMutex(psgMutex);
        psgMutex = NULL;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

static void close_game_controller(void)
{
    if (gameController) {
        SDL_GameControllerClose(gameController);
        gameController = NULL;
    }
    gameControllerInstance = -1;
}

static int open_game_controller_index(int deviceIndex)
{
    if (gameController)
        return 0; /* first opened pad wins */
    if (!SDL_IsGameController(deviceIndex))
        return 0;
    SDL_GameController *pad = SDL_GameControllerOpen(deviceIndex);
    if (!pad) {
        fprintf(stderr, "SDL_GameControllerOpen(%d) failed: %s\n",
                deviceIndex, SDL_GetError());
        return 0;
    }
    SDL_Joystick *js = SDL_GameControllerGetJoystick(pad);
    if (!js) {
        SDL_GameControllerClose(pad);
        return 0;
    }
    gameController = pad;
    gameControllerInstance = SDL_JoystickInstanceID(js);
    return 1;
}

static void open_first_game_controller(void)
{
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (open_game_controller_index(i))
            return;
    }
}

static void shutdown_pc(void)
{
    close_game_controller();
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    close_audio();
    if (texture)
        SDL_DestroyTexture(texture);
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    texture = NULL;
    renderer = NULL;
    window = NULL;
    SDL_Quit();
}

static void poll_events(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            running = 0;
        else if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
            running = 0;
        else if (event.type == SDL_CONTROLLERDEVICEADDED)
            open_game_controller_index(event.cdevice.which);
        else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
            if (event.cdevice.which == gameControllerInstance)
                close_game_controller();
        }
    }
}

/* Map the active gamepad into SGDK button bits. Returns 0 when no pad is open.
 * D-pad and left stick both contribute movement; any face/shoulder/trigger/
 * stick-click is jump (BUTTON_A — game treats A|B|C as fire). Start/Back → START. */
static u16 read_game_controller(void)
{
    if (!gameController)
        return 0;

    u16 state = 0;

    if (SDL_GameControllerGetButton(gameController, SDL_CONTROLLER_BUTTON_DPAD_UP))
        state |= BUTTON_UP;
    if (SDL_GameControllerGetButton(gameController, SDL_CONTROLLER_BUTTON_DPAD_DOWN))
        state |= BUTTON_DOWN;
    if (SDL_GameControllerGetButton(gameController, SDL_CONTROLLER_BUTTON_DPAD_LEFT))
        state |= BUTTON_LEFT;
    if (SDL_GameControllerGetButton(gameController, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
        state |= BUTTON_RIGHT;

    {
        Sint16 ax = SDL_GameControllerGetAxis(gameController, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 ay = SDL_GameControllerGetAxis(gameController, SDL_CONTROLLER_AXIS_LEFTY);
        if (ay < -STICK_DEADZONE) state |= BUTTON_UP;
        if (ay >  STICK_DEADZONE) state |= BUTTON_DOWN;
        if (ax < -STICK_DEADZONE) state |= BUTTON_LEFT;
        if (ax >  STICK_DEADZONE) state |= BUTTON_RIGHT;
    }

    /* Jump: face, shoulders, stick clicks, triggers past half travel. */
    if (SDL_GameControllerGetButton(gameController, SDL_CONTROLLER_BUTTON_A)
     || SDL_GameControllerGetButton(gameController, SDL_CONTROLLER_BUTTON_B)
     || SDL_GameControllerGetButton(gameController, SDL_CONTROLLER_BUTTON_X)
     || SDL_GameControllerGetButton(gameController, SDL_CONTROLLER_BUTTON_Y)
     || SDL_GameControllerGetButton(gameController, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
     || SDL_GameControllerGetButton(gameController, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
     || SDL_GameControllerGetButton(gameController, SDL_CONTROLLER_BUTTON_LEFTSTICK)
     || SDL_GameControllerGetButton(gameController, SDL_CONTROLLER_BUTTON_RIGHTSTICK)
     || SDL_GameControllerGetAxis(gameController, SDL_CONTROLLER_AXIS_TRIGGERLEFT)
            > TRIGGER_THRESHOLD
     || SDL_GameControllerGetAxis(gameController, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
            > TRIGGER_THRESHOLD)
        state |= BUTTON_A;

    if (SDL_GameControllerGetButton(gameController, SDL_CONTROLLER_BUTTON_START)
     || SDL_GameControllerGetButton(gameController, SDL_CONTROLLER_BUTTON_BACK))
        state |= BUTTON_START;

    return state;
}

static void print_input_debug(u16 state)
{
    fprintf(stderr,
            "[input] pad=%s  U=%d D=%d L=%d R=%d  jump=%d start=%d  raw=0x%04X\n",
            gameController ? "yes" : "no",
            (state & BUTTON_UP)    ? 1 : 0,
            (state & BUTTON_DOWN)  ? 1 : 0,
            (state & BUTTON_LEFT)  ? 1 : 0,
            (state & BUTTON_RIGHT) ? 1 : 0,
            (state & (BUTTON_A | BUTTON_B | BUTTON_C)) ? 1 : 0,
            (state & BUTTON_START) ? 1 : 0,
            (unsigned)state);
}

static int save_screenshot(const char *path)
{
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(
        (void *)(mdMode ? pixels : fullcolorPixels), textureW, textureH, 32,
        textureW * (int)sizeof(u32),
        SDL_PIXELFORMAT_ARGB8888);
    if (!surface) {
        fprintf(stderr, "Could not create screenshot surface: %s\n", SDL_GetError());
        return 0;
    }
    if (SDL_SaveBMP(surface, path) != 0) {
        fprintf(stderr, "Could not save screenshot '%s': %s\n", path, SDL_GetError());
        SDL_FreeSurface(surface);
        return 0;
    }
    SDL_FreeSurface(surface);
    return 1;
}

int SGDK_PC_init(int argc, char **argv)
{
    int scale = 4;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--md") == 0) {
            mdMode = 1;
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = atoi(argv[++i]);
            if (scale != 3 && scale != 4) {
                fprintf(stderr, "--scale must be 3 or 4\n");
                return 0;
            }
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshotPath = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frameLimit = atoi(argv[++i]);
            if (frameLimit < 1) {
                fprintf(stderr, "--frames must be at least 1\n");
                return 0;
            }
        } else if (strcmp(argv[i], "--dump-wav") == 0 && i + 1 < argc) {
            dumpWavPath = argv[++i];
        } else if (strcmp(argv[i], "--verify-audio") == 0) {
            verifyAudio = 1;
            if (frameLimit == 0)
                frameLimit = 280;
        } else if (strcmp(argv[i], "--input-debug") == 0) {
            inputDebug = 1;
        } else {
            fprintf(stderr,
                    "Usage: pitfall.exe [--md] [--scale 3|4] [--screenshot file.bmp] "
                    "[--frames n] [--dump-wav file.wav] [--verify-audio] "
                    "[--input-debug]\n");
            return 0;
        }
    }
    if (screenshotPath && frameLimit == 0)
        frameLimit = 2;

    /* GameController sits on the joystick subsystem and uses SDL's built-in
     * mapping DB (XInput on Windows, DualShock/DualSense, generic pads). */
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL gamecontroller subsystem failed: %s\n", SDL_GetError());
        /* Non-fatal: keyboard still works. */
    } else {
        open_first_game_controller();
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    textureW = mdMode ? MD_SCREEN_W : PC_SCREEN_W;
    textureH = mdMode ? MD_SCREEN_H : PC_SCREEN_H;
    window = SDL_CreateWindow(mdMode ? "Pitfall! - Mega Drive renderer"
                                     : "Pitfall! - Full-colour PC renderer",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              MD_SCREEN_W * scale, MD_SCREEN_H * scale,
                              SDL_WINDOW_SHOWN);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 0;
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_RenderSetLogicalSize(renderer, textureW, textureH);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, textureW, textureH);
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return 0;
    }

    if (dumpWavPath && !open_dump_wav(dumpWavPath))
        return 0;
    if (!open_audio())
        fprintf(stderr, "Continuing without audio.\n");

    perfFrequency = SDL_GetPerformanceFrequency();
    nextFrame = SDL_GetPerformanceCounter();
    statsStart = nextFrame;
    lastInputDebugTick = nextFrame;
    atexit(shutdown_pc);
    return 1;
}

void SGDK_PC_PSG_setTone(u8 channel, u16 value)
{
    if (psgMutex)
        SDL_LockMutex(psgMutex);
    PSG_setTone(channel, value);
    if (psgMutex)
        SDL_UnlockMutex(psgMutex);
}

void SGDK_PC_PSG_setEnvelope(u8 channel, u8 value)
{
    if (psgMutex)
        SDL_LockMutex(psgMutex);
    PSG_setEnvelope(channel, value);
    if (psgMutex)
        SDL_UnlockMutex(psgMutex);
}

void SGDK_PC_PSG_setNoise(u8 type, u8 frequency)
{
    if (psgMutex)
        SDL_LockMutex(psgMutex);
    PSG_setNoise(type, frequency);
    if (psgMutex)
        SDL_UnlockMutex(psgMutex);
}

void VDP_setScreenWidth320(void)
{
}

void VDP_setBackgroundColor(u8 index)
{
    backdropIndex = index & 63u;
}

void VDP_loadTileData(const u32 *data, u16 index, u16 num, TransferMethod tm)
{
    (void)tm;
    if (index >= TILE_COUNT)
        return;
    if ((u32)index + num > TILE_COUNT)
        num = (u16)(TILE_COUNT - index);
    memcpy(&vram[index][0], data, (size_t)num * 8u * sizeof(u32));
}

void VDP_setTileMapDataRow(VDPPlane plane, const u16 *row, u16 rowIdx,
                           u16 x, u16 len, TransferMethod tm)
{
    (void)tm;
    if (plane > BG_B || rowIdx >= PLANE_H || x >= PLANE_W)
        return;
    if ((u32)x + len > PLANE_W)
        len = (u16)(PLANE_W - x);
    memcpy(&planes[plane][rowIdx][x], row, (size_t)len * sizeof(u16));
}

void VDP_fillTileMapRect(VDPPlane plane, u16 attr, u16 x, u16 y, u16 w, u16 h)
{
    if (plane > BG_B)
        return;
    for (u16 row = 0; row < h && y + row < PLANE_H; row++)
        for (u16 col = 0; col < w && x + col < PLANE_W; col++)
            planes[plane][y + row][x + col] = attr;
}

void VDP_setSpriteFull(u16 index, s16 x, s16 y, u8 size, u16 attr, u16 link)
{
    if (index >= MAX_SPRITES)
        return;
    spriteWork[index].x = x;
    spriteWork[index].y = y;
    spriteWork[index].size = size;
    spriteWork[index].attr = attr;
    spriteWork[index].link = link;
}

void VDP_updateSprites(u16 num, TransferMethod tm)
{
    (void)tm;
    if (num > MAX_SPRITES)
        num = MAX_SPRITES;
    memcpy(sprites, spriteWork, (size_t)num * sizeof(PcSprite));
    spriteCount = num;
}

void VDP_setScrollingMode(u16 h, u16 v)
{
    (void)h;
    (void)v;
}

void VDP_setHorizontalScrollTile(VDPPlane plane, u16 tile, s16 *values,
                                 u16 len, TransferMethod tm)
{
    (void)tm;
    if (plane > BG_B || tile >= PLANE_H)
        return;
    if ((u32)tile + len > PLANE_H)
        len = (u16)(PLANE_H - tile);
    memcpy(&hscroll[plane][tile], values, (size_t)len * sizeof(s16));
}

void PAL_setColors(u16 index, const u16 *pal, u16 count, TransferMethod tm)
{
    (void)tm;
    if (index >= 64)
        return;
    if ((u32)index + count > 64)
        count = (u16)(64 - index);
    memcpy(&cram[index], pal, (size_t)count * sizeof(u16));
}

u16 JOY_readJoypad(u16 joy)
{
    (void)joy;
    poll_events();
    SDL_PumpEvents();
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    u16 state = 0;
    if (keys[SDL_SCANCODE_UP])     state |= BUTTON_UP;
    if (keys[SDL_SCANCODE_DOWN])   state |= BUTTON_DOWN;
    if (keys[SDL_SCANCODE_LEFT])   state |= BUTTON_LEFT;
    if (keys[SDL_SCANCODE_RIGHT])  state |= BUTTON_RIGHT;
    if (keys[SDL_SCANCODE_Z])      state |= BUTTON_A;
    if (keys[SDL_SCANCODE_X])      state |= BUTTON_B;
    if (keys[SDL_SCANCODE_C])      state |= BUTTON_C;
    if (keys[SDL_SCANCODE_RETURN]) state |= BUTTON_START;

    /* Gamepad bits OR with keyboard so either input path can drive Harry. */
    state |= read_game_controller();

    if (inputDebug && perfFrequency) {
        uint64_t now = SDL_GetPerformanceCounter();
        if (now - lastInputDebugTick >= perfFrequency) {
            print_input_debug(state);
            lastInputDebugTick = now;
        }
    }
    return state;
}

void SYS_doVBlankProcess(void)
{
    if (mdMode)
        compose_frame();
    if (!mdMode && !fullcolorPixels) {
        fprintf(stderr, "Full-colour renderer did not submit a frame.\n");
        exit(1);
    }
    SDL_UpdateTexture(texture, NULL, mdMode ? pixels : fullcolorPixels,
                      textureW * (int)sizeof(u32));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    frameNumber++;
    /* Cue the same soundIdx values the game uses; sound_frame() picks them
     * up on the next frame and drives PSG_set* through the locked wrappers. */
    if (verifyAudio) {
        if (frameNumber == 10) {
            soundIdx = 0x20; /* SOUND_JUMP */
            soundDelay = 0;
        } else if (frameNumber == 50) {
            soundIdx = 0x25; /* SOUND_TREASURE */
            soundDelay = 0;
        } else if (frameNumber == 120) {
            soundIdx = 0x31; /* SOUND_DEAD */
            soundDelay = 0;
        }
    }
    if (frameLimit && frameNumber >= frameLimit) {
        int ok = !screenshotPath || save_screenshot(screenshotPath);
        uint64_t elapsed = SDL_GetPerformanceCounter() - statsStart;
        double seconds = perfFrequency ? (double)elapsed / (double)perfFrequency : 0.0;
        double fps = (seconds > 0.0 && frameNumber > 1)
                   ? (double)(frameNumber - 1) / seconds : 0.0;
        fprintf(stderr, "Renderer: %s, %dx%d; %d frames in %.3f s (%.2f fps)\n",
                mdMode ? "md" : "pc", textureW, textureH,
                frameNumber, seconds, fps);
        exit(ok ? 0 : 1);
    }

    poll_events();
    if (!running)
        exit(0);

    /* Rational 60 Hz deadline: carry the division remainder to avoid drift. */
    nextFrame += perfFrequency / 60u;
    frameRemainder += perfFrequency % 60u;
    if (frameRemainder >= 60u) {
        nextFrame++;
        frameRemainder -= 60u;
    }
    for (;;) {
        uint64_t now = SDL_GetPerformanceCounter();
        if (now >= nextFrame)
            break;
        uint64_t remainingMs = (nextFrame - now) * 1000u / perfFrequency;
        if (remainingMs > 1u)
            SDL_Delay((Uint32)(remainingMs - 1u));
        else
            SDL_Delay(0);
    }
}

int SGDK_PC_is_md(void)
{
    return mdMode;
}

void SGDK_PC_set_fullcolor_frame(const u32 *frame, u16 width, u16 height)
{
    if (width != PC_SCREEN_W || height != PC_SCREEN_H) {
        fprintf(stderr, "Bad full-colour frame size: %ux%u\n",
                (unsigned)width, (unsigned)height);
        exit(1);
    }
    fullcolorPixels = frame;
}

u16 SGDK_PC_getVCounter(void)
{
    if (!perfFrequency)
        return 0;
    uint64_t elapsed = SDL_GetPerformanceCounter() - (nextFrame - perfFrequency / 60u);
    return (u16)((elapsed * 262u * 60u / perfFrequency) % 262u);
}
#endif
