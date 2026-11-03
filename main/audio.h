// Microphone capture (onboard PDM mic) and speaker playback (MAX98357A).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Called from the audio sender task with ~100 ms of 24 kHz mono PCM.
typedef void (*audio_mic_cb_t)(const int16_t *pcm, size_t samples);

typedef enum {
    CHIRP_HELLO,    // connected and ready
    CHIRP_WAKE,     // started listening
    CHIRP_CANCEL,   // stopped listening
    CHIRP_ERROR,    // something went wrong
    CHIRP_SHUTTER,  // photo taken
} chirp_t;

#define CHIRP_WAKE_MS 120

esp_err_t audio_init(audio_mic_cb_t mic_cb);

// Microphone: when on, audio is passed to mic_cb in 100 ms chunks.
// Turning it off waits (up to ~300 ms) until the last partial chunk is sent.
void audio_mic_stream(bool on);

// Speaker: queue 24 kHz mono PCM for playback. Safe from any task.
void audio_play_pcm(const int16_t *pcm, size_t samples);
void audio_play_chirp(chirp_t chirp);
void audio_flush(void);          // drop everything queued, right now
void audio_end_of_stream(void);  // post EV_PLAYBACK_DRAINED once the queue empties

// 0..1 loudness of what is playing right now; drives the beak.
float audio_get_level(void);
