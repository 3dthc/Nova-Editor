/* The ring nova-audio fills with MPC's audio inputs, read by Nova Editor in the same process.
 * One writer (MPC's audio thread, in snd_pcm_readi/readn), any number of readers. A reader keeps its own position
 * and reads frames [rpos, wpos); wpos only grows. Samples are int32 full scale. */
#pragma once
#include <stdatomic.h>
#include <stdint.h>

#define NOVA_AUDIO_VERSION "0.1.0"
#define NOVA_TAP_MAGIC 0x4e4f5641u   /* 'NOVA' */
#define NOVA_TAP_VERSION 1
#define NOVA_TAP_FRAMES 8192         /* power of two */
#define NOVA_TAP_MAX_CH 8

typedef struct {
    uint32_t magic, version;
    _Atomic int ok;                  /* MPC's capture stream is known and tapped */
    unsigned ch, rate;               /* MPC's input channels and sample rate */
    _Atomic uint64_t wpos;           /* frames written so far */
    int32_t ring[NOVA_TAP_FRAMES][NOVA_TAP_MAX_CH];
} nova_tap_t;

typedef nova_tap_t *(*nova_audio_tap_fn)(void);
