// OpenAI Realtime API client over WebSocket.
//
// Protocol (all JSON text frames):
//   device -> server: session.update, input_audio_buffer.append/clear/commit,
//                     response.create, response.cancel, conversation.item.create
//   server -> device: session.created/updated, input_audio_buffer.speech_started/
//                     speech_stopped, response.created, response.output_audio.delta,
//                     response.output_audio_transcript.done, response.done, error
//
// Audio both ways is base64-encoded 24 kHz 16-bit mono PCM.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

#include "app.h"
#include "audio.h"
#include "auth.h"
#include "camera.h"
#include "persona.h"
#include "realtime.h"
#include "wifi.h"

static const char *TAG = "realtime";

#define RT_URL_BASE      "wss://api.openai.com/v1/realtime?model="
#define BIT_CONNECTED    BIT0
#define BIT_CLOSED       BIT1

static esp_websocket_client_handle_t s_ws;
static SemaphoreHandle_t s_ws_mutex;   // guards s_ws against teardown
static EventGroupHandle_t s_events;
static volatile bool s_ready;

// Receive reassembly (messages can be split across many DATA events).
static char *s_rx;
static size_t s_rx_cap;
static size_t s_rx_len;
static bool s_rx_active;

// Scratch buffers.
static char *s_audio_json;
static size_t s_audio_json_cap;
static uint8_t *s_pcm;
static size_t s_pcm_cap;

// Response tracking, for barge-in.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_active_resp[64];
static char s_cancelled_resp[64];
// Replies are only played if we asked for one: the learner started speaking,
// we committed audio, or we sent a photo. A cancel closes the gate, so a reply
// to speech the learner already cancelled gets dropped even if it starts late.
static bool s_accept_replies = true;
static bool s_audio_started;

static bool ensure_cap(void **buf, size_t *cap, size_t need)
{
    if (*cap >= need) {
        return true;
    }
    size_t new_cap = need + need / 2;
    void *p = heap_caps_realloc(*buf, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGE(TAG, "out of PSRAM (%u bytes)", (unsigned)new_cap);
        return false;
    }
    *buf = p;
    *cap = new_cap;
    return true;
}

// ---- Sending ----------------------------------------------------------------------

// From the websocket task itself (event handler): the client is alive, no lock needed.
static bool send_from_handler(const char *data, size_t len)
{
    return s_ws && esp_websocket_client_send_text(s_ws, data, len, pdMS_TO_TICKS(3000)) >= 0;
}

// From any other task.
static bool send_raw(const char *data, size_t len)
{
    bool ok = false;
    if (xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return false;
    }
    if (s_ws && esp_websocket_client_is_connected(s_ws)) {
        ok = esp_websocket_client_send_text(s_ws, data, len, pdMS_TO_TICKS(3000)) >= 0;
        if (!ok) {
            ESP_LOGW(TAG, "send failed (%u bytes)", (unsigned)len);
        }
    }
    xSemaphoreGive(s_ws_mutex);
    return ok;
}

static bool send_str(const char *s)
{
    return send_raw(s, strlen(s));
}

static bool send_json(cJSON *obj, bool from_handler)
{
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) {
        return false;
    }
    bool ok = from_handler ? send_from_handler(s, strlen(s)) : send_str(s);
    cJSON_free(s);
    return ok;
}

static void send_session_update(void)
{
    char *instructions = persona_build_instructions();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON *s = cJSON_AddObjectToObject(root, "session");
    cJSON_AddStringToObject(s, "type", "realtime");
    cJSON_AddStringToObject(s, "instructions", instructions ? instructions : "");
    cJSON *mods = cJSON_AddArrayToObject(s, "output_modalities");
    cJSON_AddItemToArray(mods, cJSON_CreateString("audio"));

    cJSON *audio = cJSON_AddObjectToObject(s, "audio");
    cJSON *in = cJSON_AddObjectToObject(audio, "input");
    cJSON *in_fmt = cJSON_AddObjectToObject(in, "format");
    cJSON_AddStringToObject(in_fmt, "type", "audio/pcm");
    cJSON_AddNumberToObject(in_fmt, "rate", 24000);
    cJSON *nr = cJSON_AddObjectToObject(in, "noise_reduction");
    cJSON_AddStringToObject(nr, "type", "near_field");
#if CONFIG_CALIOS_TALK_HOLD
    cJSON_AddNullToObject(in, "turn_detection");  // we commit manually on release
#else
    cJSON *td = cJSON_AddObjectToObject(in, "turn_detection");
#if CONFIG_CALIOS_VAD_SEMANTIC
    cJSON_AddStringToObject(td, "type", "semantic_vad");
    cJSON_AddStringToObject(td, "eagerness", "low");  // learners pause to think
#else
    cJSON_AddStringToObject(td, "type", "server_vad");
    cJSON_AddNumberToObject(td, "threshold", 0.5);
    cJSON_AddNumberToObject(td, "prefix_padding_ms", 300);
    cJSON_AddNumberToObject(td, "silence_duration_ms", 800);
#endif
    cJSON_AddTrueToObject(td, "create_response");
    cJSON_AddFalseToObject(td, "interrupt_response");
#endif
#if CONFIG_CALIOS_LOG_USER_TRANSCRIPT
    cJSON *tr = cJSON_AddObjectToObject(in, "transcription");
    cJSON_AddStringToObject(tr, "model", "gpt-4o-mini-transcribe");
#endif

    cJSON *out = cJSON_AddObjectToObject(audio, "output");
    cJSON *out_fmt = cJSON_AddObjectToObject(out, "format");
    cJSON_AddStringToObject(out_fmt, "type", "audio/pcm");
    cJSON_AddNumberToObject(out_fmt, "rate", 24000);
    cJSON_AddStringToObject(out, "voice", CONFIG_CALIOS_VOICE);

    cJSON *tools = cJSON_AddArrayToObject(s, "tools");
    if (camera_available()) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON_AddStringToObject(tool, "name", "take_photo");
        cJSON_AddStringToObject(tool, "description",
                                "Take a photo with your camera to see what the learner is looking at or "
                                "pointing you toward. Use it when they ask about something in front of them.");
        cJSON *params = cJSON_AddObjectToObject(tool, "parameters");
        cJSON_AddStringToObject(params, "type", "object");
        cJSON_AddObjectToObject(params, "properties");
        cJSON_AddItemToArray(tools, tool);
    }
    cJSON_AddStringToObject(s, "tool_choice", "auto");

    send_json(root, true);
    cJSON_Delete(root);
    free(instructions);
}

void realtime_send_audio(const int16_t *pcm, size_t samples)
{
    if (!s_ready) {
        return;
    }
    static const char pre[] = "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
    static const char post[] = "\"}";
    const size_t bytes = samples * 2;
    const size_t b64_len = 4 * ((bytes + 2) / 3);
    const size_t need = sizeof(pre) - 1 + b64_len + sizeof(post) + 1;
    if (!ensure_cap((void **)&s_audio_json, &s_audio_json_cap, need)) {
        return;
    }
    size_t pos = sizeof(pre) - 1;
    memcpy(s_audio_json, pre, pos);
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)s_audio_json + pos, s_audio_json_cap - pos, &olen,
                              (const unsigned char *)pcm, bytes) != 0) {
        return;
    }
    pos += olen;
    memcpy(s_audio_json + pos, post, sizeof(post));  // includes NUL
    pos += sizeof(post) - 1;
    send_raw(s_audio_json, pos);
}

void realtime_clear_input(void)
{
    send_str("{\"type\":\"input_audio_buffer.clear\"}");
}

static void open_reply_gate(void)
{
    portENTER_CRITICAL(&s_lock);
    s_accept_replies = true;
    portEXIT_CRITICAL(&s_lock);
}

void realtime_commit_and_respond(void)
{
    open_reply_gate();
    send_str("{\"type\":\"input_audio_buffer.commit\"}");
    send_str("{\"type\":\"response.create\"}");
}

void realtime_cancel_response(void)
{
    bool send = false;
    portENTER_CRITICAL(&s_lock);
    s_accept_replies = false;
    if (s_active_resp[0]) {
        strlcpy(s_cancelled_resp, s_active_resp, sizeof(s_cancelled_resp));
        send = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (send) {
        send_str("{\"type\":\"response.cancel\"}");
    }
}

static void send_function_output(const char *call_id, const char *output_json)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "conversation.item.create");
    cJSON *item = cJSON_AddObjectToObject(root, "item");
    cJSON_AddStringToObject(item, "type", "function_call_output");
    cJSON_AddStringToObject(item, "call_id", call_id);
    cJSON_AddStringToObject(item, "output", output_json);
    send_json(root, false);
    cJSON_Delete(root);
}

void realtime_send_photo(const char *call_id, const uint8_t *jpg, size_t len, const char *question)
{
    open_reply_gate();
    if (call_id) {
        send_function_output(call_id, "{\"status\":\"photo taken, attached in the next message\"}");
    }

    static const char pre[] =
        "{\"type\":\"conversation.item.create\",\"item\":{\"type\":\"message\",\"role\":\"user\","
        "\"content\":[{\"type\":\"input_image\",\"image_url\":\"data:image/jpeg;base64,";
    static const char img_end[] = "\"}";
    static const char end[] = "]}}";

    char *text_part = NULL;
    if (question) {
        cJSON *q = cJSON_CreateString(question);
        char *quoted = cJSON_PrintUnformatted(q);  // escaped, with quotes
        cJSON_Delete(q);
        if (quoted) {
            asprintf(&text_part, ",{\"type\":\"input_text\",\"text\":%s}", quoted);
            cJSON_free(quoted);
        }
    }

    const size_t b64_len = 4 * ((len + 2) / 3);
    const size_t total = sizeof(pre) + b64_len + sizeof(img_end) + (text_part ? strlen(text_part) : 0) + sizeof(end) + 1;
    char *msg = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (msg) {
        size_t pos = sizeof(pre) - 1;
        memcpy(msg, pre, pos);
        size_t olen = 0;
        mbedtls_base64_encode((unsigned char *)msg + pos, total - pos, &olen, jpg, len);
        pos += olen;
        memcpy(msg + pos, img_end, sizeof(img_end) - 1);
        pos += sizeof(img_end) - 1;
        if (text_part) {
            size_t tl = strlen(text_part);
            memcpy(msg + pos, text_part, tl);
            pos += tl;
        }
        memcpy(msg + pos, end, sizeof(end));
        pos += sizeof(end) - 1;
        ESP_LOGI(TAG, "sending photo (%u bytes of JSON)", (unsigned)pos);
        send_raw(msg, pos);
        free(msg);
    }
    free(text_part);
    send_str("{\"type\":\"response.create\"}");
}

void realtime_send_photo_failed(const char *call_id)
{
    open_reply_gate();
    send_function_output(call_id, "{\"error\":\"the camera is not working right now\"}");
    send_str("{\"type\":\"response.create\"}");
}

// ---- Receiving --------------------------------------------------------------------

static bool is_cancelled(const char *resp_id)
{
    bool c;
    portENTER_CRITICAL(&s_lock);
    c = resp_id && s_cancelled_resp[0] && strcmp(resp_id, s_cancelled_resp) == 0;
    portEXIT_CRITICAL(&s_lock);
    return c;
}

static void on_audio_delta(cJSON *root)
{
    const char *rid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "response_id"));
    if (is_cancelled(rid)) {
        return;
    }
    const char *b64 = cJSON_GetStringValue(cJSON_GetObjectItem(root, "delta"));
    if (!b64) {
        return;
    }
    size_t b64_len = strlen(b64);
    if (!ensure_cap((void **)&s_pcm, &s_pcm_cap, b64_len / 4 * 3 + 4)) {
        return;
    }
    size_t olen = 0;
    if (mbedtls_base64_decode(s_pcm, s_pcm_cap, &olen, (const unsigned char *)b64, b64_len) != 0) {
        ESP_LOGW(TAG, "bad base64 in audio delta");
        return;
    }
    if (!s_audio_started) {
        s_audio_started = true;
        app_post(EV_AUDIO_STARTED, 0, NULL);
    }
    audio_play_pcm((const int16_t *)s_pcm, olen / 2);
}

static void on_response_created(cJSON *root)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "response"), "id"));
    if (!id) {
        return;
    }
    bool cancel_now = false;
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_active_resp, id, sizeof(s_active_resp));
    if (!s_accept_replies) {
        strlcpy(s_cancelled_resp, id, sizeof(s_cancelled_resp));
        cancel_now = true;
    }
    portEXIT_CRITICAL(&s_lock);
    s_audio_started = false;
    if (cancel_now) {
        static const char cancel[] = "{\"type\":\"response.cancel\"}";
        send_from_handler(cancel, sizeof(cancel) - 1);
    }
}

static void on_response_done(cJSON *root)
{
    cJSON *resp = cJSON_GetObjectItem(root, "response");
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(resp, "id"));
    const char *status = cJSON_GetStringValue(cJSON_GetObjectItem(resp, "status"));

    bool was_cancelled = false;
    portENTER_CRITICAL(&s_lock);
    if (id && strcmp(s_active_resp, id) == 0) {
        s_active_resp[0] = 0;
    }
    if (id && s_cancelled_resp[0] && strcmp(s_cancelled_resp, id) == 0) {
        s_cancelled_resp[0] = 0;
        was_cancelled = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (was_cancelled || (status && strcmp(status, "cancelled") == 0)) {
        ESP_LOGI(TAG, "reply cancelled");
        return;
    }
    if (status && strcmp(status, "completed") != 0) {
        char *details = cJSON_PrintUnformatted(cJSON_GetObjectItem(resp, "status_details"));
        ESP_LOGW(TAG, "reply ended with status %s: %s", status, details ? details : "");
        cJSON_free(details);
    }

    cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(resp, "output")) {
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(item, "type"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
        const char *call_id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "call_id"));
        if (type && strcmp(type, "function_call") == 0 && call_id) {
            if (name && strcmp(name, "take_photo") == 0) {
                ESP_LOGI(TAG, "model asked for a photo");
                app_post(EV_TOOL_PHOTO, 0, call_id);
                return;
            }
            ESP_LOGW(TAG, "unknown tool %s", name ? name : "?");
        }
    }
    app_post(EV_RESPONSE_DONE, 0, NULL);
}

static void handle_message(const char *msg, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(msg, len);
    if (!root) {
        ESP_LOGW(TAG, "unparseable message (%u bytes)", (unsigned)len);
        return;
    }
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    if (!type) {
        cJSON_Delete(root);
        return;
    }

    if (!strcmp(type, "response.output_audio.delta") || !strcmp(type, "response.audio.delta")) {
        on_audio_delta(root);
    } else if (!strcmp(type, "session.created")) {
        ESP_LOGI(TAG, "session created, configuring");
        send_session_update();
    } else if (!strcmp(type, "session.updated")) {
        if (!s_ready) {
            s_ready = true;
            ESP_LOGI(TAG, "session ready");
            app_post(EV_RT_READY, 0, NULL);
        }
    } else if (!strcmp(type, "input_audio_buffer.speech_started")) {
        open_reply_gate();
        app_post(EV_SPEECH_STARTED, 0, NULL);
    } else if (!strcmp(type, "input_audio_buffer.speech_stopped")) {
        app_post(EV_SPEECH_STOPPED, 0, NULL);
    } else if (!strcmp(type, "response.created")) {
        on_response_created(root);
    } else if (!strcmp(type, "response.done")) {
        on_response_done(root);
    } else if (!strcmp(type, "response.output_audio_transcript.done") ||
               !strcmp(type, "response.audio_transcript.done")) {
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(root, "transcript"));
        ESP_LOGI(TAG, "%s: %s", CONFIG_CALIOS_PARROT_NAME, t ? t : "");
    } else if (!strcmp(type, "conversation.item.input_audio_transcription.completed")) {
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(root, "transcript"));
        ESP_LOGI(TAG, "learner: %s", t ? t : "");
    } else if (!strcmp(type, "error")) {
        cJSON *err = cJSON_GetObjectItem(root, "error");
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(err, "code"));
        const char *message = cJSON_GetStringValue(cJSON_GetObjectItem(err, "message"));
        if (code && !strcmp(code, "response_cancel_not_active")) {
            // harmless: we tried to cancel a reply that had already finished
        } else {
            ESP_LOGE(TAG, "server error %s: %s", code ? code : "?", message ? message : "");
            app_post(EV_RT_ERROR, 0, NULL);
        }
    }
    cJSON_Delete(root);
}

static void on_data(const esp_websocket_event_data_t *d)
{
    if (d->op_code == 0x08) {  // close
        if (d->data_len >= 2) {
            int code = ((uint8_t)d->data_ptr[0] << 8) | (uint8_t)d->data_ptr[1];
            ESP_LOGW(TAG, "server closed the connection: %d %.*s", code, d->data_len - 2, d->data_ptr + 2);
        }
        return;
    }
    if (d->op_code == 0x01 && d->payload_offset == 0) {
        s_rx_len = 0;
        s_rx_active = true;
    } else if (d->op_code == 0x00) {
        if (!s_rx_active) {
            return;
        }
    } else if (d->op_code != 0x01) {
        return;  // ping/pong/binary
    }

    if (d->data_len > 0) {
        if (!ensure_cap((void **)&s_rx, &s_rx_cap, s_rx_len + d->data_len + 1)) {
            s_rx_active = false;
            return;
        }
        memcpy(s_rx + s_rx_len, d->data_ptr, d->data_len);
        s_rx_len += d->data_len;
    }
    bool frame_done = d->payload_offset + d->data_len >= d->payload_len;
    if (frame_done && d->fin) {
        s_rx[s_rx_len] = 0;
        handle_message(s_rx, s_rx_len);
        s_rx_len = 0;
        s_rx_active = false;
    }
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to OpenAI");
        xEventGroupSetBits(s_events, BIT_CONNECTED);
        break;
    case WEBSOCKET_EVENT_DATA:
        on_data(d);
        break;
    case WEBSOCKET_EVENT_ERROR:
        if (d->error_handle.esp_ws_handshake_status_code == 401) {
            ESP_LOGE(TAG, "401 Unauthorized: check the OpenAI API key in menuconfig");
        } else if (d->error_handle.esp_ws_handshake_status_code) {
            ESP_LOGE(TAG, "handshake failed with HTTP %d", d->error_handle.esp_ws_handshake_status_code);
        } else {
            ESP_LOGE(TAG, "websocket error");
        }
        xEventGroupSetBits(s_events, BIT_CLOSED);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "disconnected");
        xEventGroupSetBits(s_events, BIT_CLOSED);
        break;
    default:
        break;
    }
}

// ---- Connection manager ---------------------------------------------------------

static void realtime_task(void *arg)
{
    char *key = heap_caps_calloc(1, 512, MALLOC_CAP_SPIRAM);
    char *headers = heap_caps_calloc(1, 640, MALLOC_CAP_SPIRAM);
    char uri[160];
    snprintf(uri, sizeof(uri), RT_URL_BASE "%s", CONFIG_CALIOS_REALTIME_MODEL);
    int backoff_ms = 2000;

    for (;;) {
        wifi_wait_connected(portMAX_DELAY);

        if (auth_get_key(key, 512) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = backoff_ms * 2 > 30000 ? 30000 : backoff_ms * 2;
            continue;
        }
        snprintf(headers, 640, "Authorization: Bearer %s\r\n", key);
        memset(key, 0, 512);

        esp_websocket_client_config_t cfg = {
            .uri = uri,
            .headers = headers,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = 8192,
            .task_stack = 10240,
            .task_prio = 6,
            .network_timeout_ms = 15000,
            .disable_auto_reconnect = true,
        };
        xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_CLOSED);
        s_rx_len = 0;
        s_rx_active = false;
        portENTER_CRITICAL(&s_lock);
        s_active_resp[0] = 0;
        s_cancelled_resp[0] = 0;
        s_accept_replies = true;
        portEXIT_CRITICAL(&s_lock);

        ESP_LOGI(TAG, "connecting to %s", uri);
        esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg);
        esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_event, NULL);
        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        s_ws = client;
        xSemaphoreGive(s_ws_mutex);
        esp_websocket_client_start(client);

        EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_CLOSED, pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(20000));
        if (bits & BIT_CONNECTED) {
            backoff_ms = 2000;
            xEventGroupWaitBits(s_events, BIT_CLOSED, pdFALSE, pdFALSE, portMAX_DELAY);
        } else if (!(bits & BIT_CLOSED)) {
            ESP_LOGW(TAG, "connect timed out");
        }

        s_ready = false;
        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        s_ws = NULL;
        xSemaphoreGive(s_ws_mutex);
        esp_websocket_client_destroy(client);
        app_post(EV_RT_CLOSED, 0, NULL);

        ESP_LOGI(TAG, "reconnecting in %d ms", backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms = backoff_ms * 2 > 30000 ? 30000 : backoff_ms * 2;
    }
}

void realtime_start(void)
{
    s_ws_mutex = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    xTaskCreatePinnedToCore(realtime_task, "realtime", 6144, NULL, 5, NULL, 0);
}
