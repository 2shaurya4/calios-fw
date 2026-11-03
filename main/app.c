// Parrot state machine. All decisions about what the parrot does next live here.
//
// Tap mode (default):
//   IDLE --tap--> LISTENING --service hears you stop--> THINKING --first audio--> SPEAKING
//   SPEAKING --reply finished playing--> LISTENING again for a short follow-up window
//   --no speech--> IDLE
//   Tapping while the parrot is thinking or talking interrupts it and listens.
//   Holding the head ~1.2 s while idle takes a photo and asks about it.
//
// Hold mode: hold the head while you talk, let go to send.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "app.h"
#include "audio.h"
#include "camera.h"
#include "leds.h"
#include "realtime.h"
#include "servos.h"

static const char *TAG = "app";

#define LONG_PRESS_MS        1200
#define MIN_HOLD_TALK_MS     300
#define THINKING_TIMEOUT_MS  20000

static const char *PHOTO_QUESTION =
    "I'm pointing you at something. What is it? Teach me the word for the main thing you see.";

typedef enum { DL_NONE, DL_NO_SPEECH, DL_MAX_LISTEN, DL_THINKING } deadline_kind_t;

static QueueHandle_t s_queue;
static parrot_state_t s_state = PS_BOOT;
static bool s_wifi_up;
static bool s_rt_ready;
static deadline_kind_t s_deadline_kind = DL_NONE;
static int64_t s_deadline_us;

static const char *state_name(parrot_state_t s)
{
    static const char *names[] = {"BOOT", "WIFI", "CONNECTING", "IDLE", "LISTENING", "THINKING", "SPEAKING"};
    return (s <= PS_SPEAKING) ? names[s] : "?";
}

static inline bool hold_mode(void)
{
#if CONFIG_CALIOS_TALK_HOLD
    return true;
#else
    return false;
#endif
}

void app_init(void)
{
    s_queue = xQueueCreate(32, sizeof(app_event_t));
}

void app_post(app_event_type_t type, uint32_t arg, const char *text)
{
    if (!s_queue) {
        return;
    }
    app_event_t ev = {.type = type, .arg = arg};
    if (text) {
        strlcpy(ev.text, text, sizeof(ev.text));
    }
    if (xQueueSend(s_queue, &ev, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped event %d", type);
    }
}

static void set_state(parrot_state_t st)
{
    if (st == s_state) {
        return;
    }
    ESP_LOGI(TAG, "%s -> %s", state_name(s_state), state_name(st));
    s_state = st;
    leds_set_state(st);
    servos_set_state(st);
}

static void set_deadline(deadline_kind_t kind, uint32_t ms)
{
    s_deadline_kind = kind;
    s_deadline_us = (kind == DL_NONE) ? 0 : esp_timer_get_time() + (int64_t)ms * 1000;
}

static void start_listening(uint32_t no_speech_ms, bool chirp)
{
    if (chirp) {
        audio_play_chirp(CHIRP_WAKE);
        // Let the chirp finish so the microphone doesn't hear it as speech.
        vTaskDelay(pdMS_TO_TICKS(CHIRP_WAKE_MS + 80));
    }
    realtime_clear_input();
    audio_mic_stream(true);
    set_state(PS_LISTENING);
    if (hold_mode() || no_speech_ms == 0) {
        set_deadline(DL_MAX_LISTEN, CONFIG_CALIOS_MAX_LISTEN_MS);
    } else {
        set_deadline(DL_NO_SPEECH, no_speech_ms);
    }
}

static void stop_listening_to_idle(bool chirp)
{
    audio_mic_stream(false);
    realtime_clear_input();
    set_deadline(DL_NONE, 0);
    if (chirp) {
        audio_play_chirp(CHIRP_CANCEL);
    }
    set_state(PS_IDLE);
}

static void go_thinking(void)
{
    audio_mic_stream(false);
    set_state(PS_THINKING);
    set_deadline(DL_THINKING, THINKING_TIMEOUT_MS);
}

static void interrupt_parrot(void)
{
    realtime_cancel_response();
    audio_flush();
}

// After a reply has finished: listen for a follow-up, or go back to idle.
static void after_reply(void)
{
    set_deadline(DL_NONE, 0);
#if CONFIG_CALIOS_TALK_TAP
    if (CONFIG_CALIOS_FOLLOWUP_LISTEN_MS > 0 && s_rt_ready) {
        start_listening(CONFIG_CALIOS_FOLLOWUP_LISTEN_MS, false);
        return;
    }
#endif
    set_state(PS_IDLE);
}

// call_id != NULL: the model asked for a photo through its take_photo tool.
// call_id == NULL: the learner long-pressed the head to ask about something.
static void take_photo_and_send(const char *call_id)
{
    set_state(PS_THINKING);
    set_deadline(DL_THINKING, THINKING_TIMEOUT_MS);

    uint8_t *jpg = NULL;
    size_t len = 0;
    leds_show_privacy(1500);
    audio_play_chirp(CHIRP_SHUTTER);
    esp_err_t err = camera_capture(&jpg, &len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "photo captured: %u bytes", (unsigned)len);
        realtime_send_photo(call_id, jpg, len, call_id ? NULL : PHOTO_QUESTION);
        free(jpg);
        return;
    }
    ESP_LOGW(TAG, "photo failed: %s", esp_err_to_name(err));
    if (call_id) {
        realtime_send_photo_failed(call_id);
    } else {
        audio_play_chirp(CHIRP_ERROR);
        leds_flash_error();
        set_deadline(DL_NONE, 0);
        set_state(PS_IDLE);
    }
}

static bool require_ready(void)
{
    if (s_rt_ready) {
        return true;
    }
    ESP_LOGW(TAG, "not connected yet, ignoring input");
    audio_play_chirp(CHIRP_ERROR);
    leds_flash_error();
    return false;
}

// ---- Tap mode ---------------------------------------------------------------

static void on_tap_release(uint32_t press_ms)
{
    if (!require_ready()) {
        return;
    }
    switch (s_state) {
    case PS_IDLE:
        if (press_ms >= LONG_PRESS_MS && camera_available()) {
            take_photo_and_send(NULL);
        } else {
            start_listening(CONFIG_CALIOS_NO_SPEECH_TIMEOUT_MS, true);
        }
        break;
    case PS_LISTENING:
        realtime_cancel_response();  // drop a reply to speech already heard
        stop_listening_to_idle(true);
        break;
    case PS_THINKING:
    case PS_SPEAKING:
        interrupt_parrot();
        start_listening(CONFIG_CALIOS_NO_SPEECH_TIMEOUT_MS, true);
        break;
    default:
        break;
    }
}

// ---- Hold mode ----------------------------------------------------------------

static void on_hold_down(void)
{
    if (!require_ready()) {
        return;
    }
    if (s_state == PS_THINKING || s_state == PS_SPEAKING) {
        interrupt_parrot();
    }
    if (s_state == PS_IDLE || s_state == PS_THINKING || s_state == PS_SPEAKING) {
        start_listening(0, false);
    }
}

static void on_hold_up(uint32_t press_ms)
{
    if (s_state != PS_LISTENING) {
        return;
    }
    if (press_ms < MIN_HOLD_TALK_MS) {
        stop_listening_to_idle(false);
        return;
    }
    audio_mic_stream(false);  // flushes the last chunk before committing
    realtime_commit_and_respond();
    go_thinking();
}

// ---- Event dispatch -------------------------------------------------------------

static void go_offline(parrot_state_t st)
{
    audio_mic_stream(false);
    audio_flush();
    set_deadline(DL_NONE, 0);
    set_state(st);
}

static void handle_event(const app_event_t *ev)
{
    switch (ev->type) {
    case EV_WIFI_UP:
        s_wifi_up = true;
        if (s_state == PS_BOOT || s_state == PS_WIFI) {
            set_state(PS_CONNECTING);
        }
        break;
    case EV_WIFI_DOWN:
        s_wifi_up = false;
        s_rt_ready = false;
        go_offline(PS_WIFI);
        break;
    case EV_RT_READY:
        s_rt_ready = true;
        if (s_state == PS_CONNECTING || s_state == PS_WIFI || s_state == PS_BOOT) {
            set_state(PS_IDLE);
            audio_play_chirp(CHIRP_HELLO);
        }
        break;
    case EV_RT_CLOSED:
        s_rt_ready = false;
        go_offline(s_wifi_up ? PS_CONNECTING : PS_WIFI);
        break;

    case EV_BTN_DOWN:
        if (hold_mode()) {
            on_hold_down();
        }
        break;
    case EV_BTN_UP:
        if (hold_mode()) {
            on_hold_up(ev->arg);
        } else {
            on_tap_release(ev->arg);
        }
        break;

    case EV_SPEECH_STARTED:
        if (s_state == PS_LISTENING) {
            set_deadline(DL_MAX_LISTEN, CONFIG_CALIOS_MAX_LISTEN_MS);
        }
        break;
    case EV_SPEECH_STOPPED:
        if (s_state == PS_LISTENING && !hold_mode()) {
            go_thinking();  // the service starts the reply on its own
        }
        break;
    case EV_AUDIO_STARTED:
        if (s_state == PS_THINKING || s_state == PS_LISTENING) {
            audio_mic_stream(false);
            set_deadline(DL_NONE, 0);
            set_state(PS_SPEAKING);
        }
        break;
    case EV_RESPONSE_DONE:
        if (s_state == PS_SPEAKING) {
            audio_end_of_stream();  // finish playing, then EV_PLAYBACK_DRAINED
        } else if (s_state == PS_THINKING) {
            after_reply();          // reply had no audio
        }
        break;
    case EV_PLAYBACK_DRAINED:
        if (s_state == PS_SPEAKING) {
            after_reply();
        }
        break;
    case EV_TOOL_PHOTO:
        if (s_state == PS_THINKING || s_state == PS_SPEAKING) {
            take_photo_and_send(ev->text);
        }
        break;
    case EV_RT_ERROR:
        leds_flash_error();
        if (s_state == PS_THINKING || (s_state == PS_LISTENING && hold_mode())) {
            audio_mic_stream(false);
            set_deadline(DL_NONE, 0);
            set_state(PS_IDLE);
        }
        break;
    }
}

static void check_deadline(void)
{
    if (s_deadline_kind == DL_NONE || esp_timer_get_time() < s_deadline_us) {
        return;
    }
    deadline_kind_t kind = s_deadline_kind;
    set_deadline(DL_NONE, 0);

    switch (kind) {
    case DL_NO_SPEECH:
        if (s_state == PS_LISTENING) {
            ESP_LOGI(TAG, "no speech heard, going idle");
            stop_listening_to_idle(false);
        }
        break;
    case DL_MAX_LISTEN:
        if (s_state == PS_LISTENING) {
            ESP_LOGI(TAG, "max listen time reached, sending what we have");
            audio_mic_stream(false);
            realtime_commit_and_respond();
            go_thinking();
        }
        break;
    case DL_THINKING:
        if (s_state == PS_THINKING) {
            ESP_LOGW(TAG, "no reply in time");
            realtime_cancel_response();
            audio_play_chirp(CHIRP_ERROR);
            leds_flash_error();
            set_state(PS_IDLE);
        }
        break;
    default:
        break;
    }
}

void app_run(void)
{
    set_state(PS_WIFI);
    app_event_t ev;
    for (;;) {
        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(50)) == pdTRUE) {
            handle_event(&ev);
        }
        check_deadline();
    }
}
