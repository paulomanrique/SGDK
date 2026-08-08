#ifdef PC_BUILD
/* Software SN76489: 3 square tone generators + 1 noise, mono 16-bit @ 44.1 kHz.
 *
 * Timing model matches the chip: tone/noise counters decrement once every
 * 16 master clocks. A tone channel reloads its 10-bit period and toggles its
 * output bit when the counter hits zero → full square period = 32*N clocks,
 * so f = CLOCK / (32 * N). Noise uses fixed periods 0x10/0x20/0x40 or the
 * current tone-2 period, and clocks a 16-bit LFSR (taps 0 and 3 for white).
 *
 * Volume uses the classic 15-step ~2 dB logarithmic attenuation table.
 */
#include "psg_pc.h"

typedef struct {
    u16 period;       /* 10-bit reload (0 stored as written; 0 → 1024 at use) */
    u16 counter;      /* current countdown */
    u8  output;       /* square level 0/1 */
    u8  volume;       /* attenuation 0..15 */
} ToneChan;

typedef struct {
    u8  type;         /* 0 periodic, 1 white */
    u8  freq;         /* 0..3 clock select */
    u16 counter;
    u16 lfsr;
    u8  output;       /* LFSR bit 0 */
    u8  volume;
} NoiseChan;

static ToneChan  tones[3];
static NoiseChan noise;
/* Rational residual: each sample adds CLOCK, each /16 tick costs RATE*16.
 * Keeps exact long-term rate without a 16.16 multiply that overflows u32. */
static u32       clock_accum;
static int       inited;

/* 2 dB/step attenuation → amplitude ≈ 4096 * 2^(-step/2); step 15 = mute.
 * Peak leaves headroom for four channels in int16. */
static const s16 vol_table[16] = {
    4096, 3254, 2584, 2053, 1631, 1295, 1029,  817,
     649,  516,  410,  325,  258,  205,  163,    0
};

static u16 tone_period(const ToneChan *t)
{
    return t->period ? t->period : 1024;
}

/* Noise countdown reload for the current control word. */
static u16 noise_period(void)
{
    /* Standard SN76489 reloads (SMS Power / every MD emulator). Counters
     * run at CLOCK/16, so LFSR rates are CLOCK/256, /512, /1024 — half the
     * TI datasheet figures often quoted as /512,/1024,/2048. */
    switch (noise.freq & 3) {
    case 0:  return 0x10;               /* ~CLOCK/256  (SGDK CLOCK2) */
    case 1:  return 0x20;               /* ~CLOCK/512  (SGDK CLOCK4) */
    case 2:  return 0x40;               /* ~CLOCK/1024 (SGDK CLOCK8) */
    default: return tone_period(&tones[2]);
    }
}

static void noise_shift(void)
{
    /* Feedback: white = bit0 XOR bit3; periodic = bit0 only.
     * 16-bit LFSR, result shifted in at bit 15. */
    u16 fb;
    if (noise.type == PSG_NOISE_TYPE_WHITE)
        fb = (noise.lfsr ^ (noise.lfsr >> 3)) & 1;
    else
        fb = noise.lfsr & 1;
    noise.lfsr = (noise.lfsr >> 1) | (fb << 15);
    noise.output = noise.lfsr & 1;
}

/* One PSG tick = 16 master clocks. */
static void psg_tick(void)
{
    int i;

    for (i = 0; i < 3; i++) {
        ToneChan *t = &tones[i];
        if (t->counter > 0)
            t->counter--;
        if (t->counter == 0) {
            t->counter = tone_period(t);
            t->output ^= 1;
        }
    }

    if (noise.counter > 0)
        noise.counter--;
    if (noise.counter == 0) {
        noise.counter = noise_period();
        noise_shift();
    }
}

void PSG_setTone(u8 channel, u16 value)
{
    if (channel > 2)
        return;
    tones[channel].period = value & 0x3FF;
    /* Do not reset the running counter — matches hardware latch behaviour. */
}

void PSG_setEnvelope(u8 channel, u8 value)
{
    value &= 0x0F;
    if (channel < 3)
        tones[channel].volume = value;
    else if (channel == 3)
        noise.volume = value;
}

void PSG_setNoise(u8 type, u8 frequency)
{
    noise.type = type ? PSG_NOISE_TYPE_WHITE : PSG_NOISE_TYPE_PERIODIC;
    noise.freq = frequency & 3;
    /* Writing the noise control register reloads the LFSR (SMS/MD behaviour). */
    noise.lfsr = 0x8000;
    noise.output = 1;
    noise.counter = noise_period();
}

void psg_pc_init(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        tones[i].period  = 0;
        tones[i].counter = 1;
        tones[i].output  = 0;
        tones[i].volume  = PSG_ENVELOPE_MIN;
    }
    noise.type     = PSG_NOISE_TYPE_WHITE;
    noise.freq     = PSG_NOISE_FREQ_CLOCK8;
    noise.counter  = noise_period();
    noise.lfsr     = 0x8000;
    noise.output   = 1;
    noise.volume   = PSG_ENVELOPE_MIN;
    clock_accum    = 0;
    inited         = 1;
}

void psg_pc_shutdown(void)
{
    inited = 0;
}

void psg_pc_render(s16 *buffer, int frames)
{
    /* One PSG tick every 16 master clocks. Per output sample we advance
     * CLOCK master clocks; scale both sides by SAMPLE_RATE so the residual
     * stays integer: accum += CLOCK, tick when accum >= RATE*16. */
    const u32 tick_cost = (u32)PSG_PC_SAMPLE_RATE * 16u;
    int f;

    if (!inited)
        psg_pc_init();

    for (f = 0; f < frames; f++) {
        s32 mix = 0;
        int i;

        clock_accum += (u32)PSG_PC_CLOCK_HZ;
        while (clock_accum >= tick_cost) {
            clock_accum -= tick_cost;
            psg_tick();
        }

        for (i = 0; i < 3; i++) {
            s16 amp = vol_table[tones[i].volume & 15];
            if (tones[i].output)
                mix += amp;
            else
                mix -= amp;
        }
        {
            s16 amp = vol_table[noise.volume & 15];
            if (noise.output)
                mix += amp;
            else
                mix -= amp;
        }

        if (mix >  32767) mix =  32767;
        if (mix < -32768) mix = -32768;
        buffer[f] = (s16)mix;
    }
}
#endif
