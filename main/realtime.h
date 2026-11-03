// Connection to the OpenAI Realtime API (speech in, speech out) over a
// WebSocket. Handles the session, streams the mic up, plays replies, and
// handles the take_photo tool.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void realtime_start(void);

// Mic callback: appends 24 kHz PCM to the server's input buffer.
void realtime_send_audio(const int16_t *pcm, size_t samples);

void realtime_clear_input(void);          // discard any audio sent so far this turn
void realtime_commit_and_respond(void);   // "I'm done talking, reply now" (hold mode)
void realtime_cancel_response(void);      // stop the reply in progress (barge-in)

// Send a JPEG to the model. call_id answers a take_photo tool call; with
// call_id == NULL the photo is sent with `question` as a new user turn.
void realtime_send_photo(const char *call_id, const uint8_t *jpg, size_t len, const char *question);
void realtime_send_photo_failed(const char *call_id);
