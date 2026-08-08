/* Software SN76489 (Mega Drive PSG) for the native PC build.
 * Exposes the SGDK PSG API the game calls, plus a render hook for SDL.
 * Pure C — no SDL dependency. */
#ifndef PSG_PC_H
#define PSG_PC_H

#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t  s16;
typedef int32_t  s32;

/* SGDK-compatible attenuation / noise constants (see tests/host_stub.h). */
#define PSG_ENVELOPE_MIN          15

#define PSG_NOISE_TYPE_PERIODIC   0
#define PSG_NOISE_TYPE_WHITE      1

/* Noise clock select (low 2 bits of the noise control latch):
 *   0 = master/512, 1 = master/1024, 2 = master/2048, 3 = tone channel 2. */
#define PSG_NOISE_FREQ_CLOCK2     0
#define PSG_NOISE_FREQ_CLOCK4     1
#define PSG_NOISE_FREQ_CLOCK8     2
#define PSG_NOISE_FREQ_TONE3      3

/* Master clock (NTSC Mega Drive PSG) and output sample rate. */
#define PSG_PC_CLOCK_HZ           3579545
#define PSG_PC_SAMPLE_RATE        44100

/* --- SGDK API surface used by src/sound.c --- */

/* 10-bit tone period on channel 0..2. Frequency = CLOCK / (32 * value).
 * value 0 is treated as 1024 (hardware quirk). */
void PSG_setTone(u8 channel, u16 value);

/* Attenuation 0..15 on channel 0..3 (0 = loudest, 15 = silent). */
void PSG_setEnvelope(u8 channel, u8 value);

/* Noise type (white/periodic) and frequency select (CLOCK2/4/8 or TONE3). */
void PSG_setNoise(u8 type, u8 frequency);

/* --- PC backend hooks (SDL audio callback pulls samples) --- */

void psg_pc_init(void);
/* Mix `frames` mono 16-bit signed samples into buffer (interleaved N/A). */
void psg_pc_render(s16 *buffer, int frames);
void psg_pc_shutdown(void);

#endif /* PSG_PC_H */
