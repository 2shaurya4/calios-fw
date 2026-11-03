// The app task: owns the parrot's state machine. Other modules report what
// happened by posting events; only the app task decides what to do next.
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PS_BOOT = 0,
    PS_WIFI,        // waiting for Wi-Fi
    PS_CONNECTING,  // Wi-Fi up, connecting to the voice service
    PS_IDLE,        // ready; tap the head to talk
    PS_LISTENING,   // streaming the microphone to the service
    PS_THINKING,    // waiting for the reply
    PS_SPEAKING,    // playing the reply, beak moving
} parrot_state_t;

typedef enum {
    EV_WIFI_UP = 1,
    EV_WIFI_DOWN,
    EV_RT_READY,          // voice session configured and ready
    EV_RT_CLOSED,         // voice session dropped
    EV_BTN_DOWN,          // head touched / button pressed
    EV_BTN_UP,            // released; arg = press duration in ms
    EV_SPEECH_STARTED,    // service heard the learner start talking
    EV_SPEECH_STOPPED,    // service decided the learner finished
    EV_AUDIO_STARTED,     // first audio of a reply arrived
    EV_RESPONSE_DONE,     // service finished sending a reply
    EV_PLAYBACK_DRAINED,  // speaker finished playing everything queued
    EV_TOOL_PHOTO,        // model asked for a photo; text = call_id
    EV_RT_ERROR,          // service reported an error
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    uint32_t arg;
    char text[64];
} app_event_t;

void app_init(void);
void app_post(app_event_type_t type, uint32_t arg, const char *text);
void app_run(void);  // never returns
