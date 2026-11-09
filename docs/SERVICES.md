# APIs and services the firmware needs

## Summary

| # | Service | Needed for | Required? | Cost |
|---|---|---|---|---|
| 1 | Wi-Fi with internet (phone hotspot works) | Everything | Yes | Your data plan |
| 2 | **OpenAI Realtime API** (`gpt-realtime-2`) | Hearing, thinking, talking, seeing photos | Yes | Per audio/image token |
| 3 | OpenAI account: project, API key, billing, budget limit | Authenticating to #2 | Yes | – |
| 4 | OpenAI transcription (`gpt-4o-mini-transcribe`) | Printing what the learner said to the log | Optional (off by default) | Per audio minute |
| 5 | Espressif component registry + ESP-IDF v5.5 | Building the firmware | Build time only | Free |

Nothing else is called. (The camera preview page is served by the parrot itself on your local Wi-Fi; it doesn't use any outside service.) There's no database, no analytics, no time server, and no Bluetooth. Speech-to-text, the language model, text-to-speech, and vision all happen inside the one Realtime session, so there's no separate STT or TTS service to set up.

## 1. Wi-Fi

- 2.4 GHz only. The ESP32-S3 can't join 5 GHz networks. On an iPhone hotspot, turn on **Maximize Compatibility**.
- WPA2 or WPA3 password networks, set in menuconfig. Networks that need a username and password login (WPA2-Enterprise, such as eduroam) or a sign-in web page **won't work**. Use a phone hotspot.
- Outbound HTTPS (TCP 443) to `api.openai.com`.
- Bandwidth: about 64 KB/s up while listening and up to about 64 KB/s down while the parrot talks (base64-encoded 24 kHz audio). That's roughly 4 MB per minute of conversation. Fine on a hotspot, but it adds up on a capped data plan.

## 2. OpenAI Realtime API

The parrot opens one WebSocket and keeps it open:

```
wss://api.openai.com/v1/realtime?model=gpt-realtime-2
Authorization: Bearer <API key>
```

Audio in both directions is **24 kHz, 16-bit, mono PCM**, base64-encoded inside JSON messages.

**Messages the parrot sends**

| Event | When |
|---|---|
| `session.update` | Right after connecting: persona instructions, audio formats, voice, turn detection, and the `take_photo` tool |
| `input_audio_buffer.append` | Every 100 ms while listening (blue LED) |
| `input_audio_buffer.clear` | At the start of each listening turn |
| `input_audio_buffer.commit` + `response.create` | Hold-to-talk release, or a turn that hits the max length |
| `response.cancel` | When you tap to interrupt |
| `conversation.item.create` (`function_call_output`) | Answering the model's `take_photo` call |
| `conversation.item.create` (`input_image`) | Sending the JPEG (as a data URL) |
| `response.create` | Asking for a reply after a photo |

**Messages the parrot handles**

| Event | What the parrot does |
|---|---|
| `session.created` / `session.updated` | Sends its settings / becomes ready (hello chirp) |
| `input_audio_buffer.speech_started` / `speech_stopped` | Knows you started talking / goes to thinking |
| `response.created` / `response.done` | Tracks the reply, finds `take_photo` calls |
| `response.output_audio.delta` | Plays the audio and moves the beak |
| `response.output_audio_transcript.done` | Prints what the parrot said to the log |
| `error` | Red LED blink, logs the message |

**Settings it sends:** semantic turn detection with low eagerness (waits for you to finish a thought instead of cutting in at short pauses), near-field noise reduction, voice `marin`, audio-only output, and one tool, `take_photo`.

**Cost:** billed per token for audio in, audio out, and images. The full conversation so far is re-read on every reply, so long sessions cost more per turn. Check current prices on OpenAI's pricing page, and **set a monthly budget limit** on the project before handing the parrot to anyone.

**Things I couldn't verify without hardware or a key:**

- The default model name `gpt-realtime-2` comes from OpenAI's current WebSocket guide. If your account doesn't have it, change it to `gpt-realtime` in menuconfig.

## 3. OpenAI account setup

1. Create an OpenAI API account (separate from a ChatGPT subscription) and add billing.
2. Create a **project** just for Calios and an API key in that project.
3. Set a **monthly budget** on the project.
4. Put the key in menuconfig → Calios parrot → Voice service → OpenAI API key.

The key is compiled into the firmware, so anyone with the parrot and a USB cable could read it. That's fine for your own demo unit, and the budget limit caps the damage. Before other people take a parrot home, add a small server that hands out short-lived keys instead (OpenAI's `POST /v1/realtime/client_secrets`).

## 4. Transcription (optional)

Turning on menuconfig → Voice service → **Also transcribe the learner** adds `audio.input.transcription` with `gpt-4o-mini-transcribe` to the session, and the log then shows what you said. It's useful for debugging and costs a bit extra.

## 5. Build-time services

- **ESP-IDF v5.5.x** (built and tested here with v5.5.6). The project refuses to build on 6.x until it's been checked there.
- **Espressif component registry** (`components.espressif.com`). `idf.py build` downloads these automatically:
  - `espressif/esp_websocket_client` ^1.6
  - `espressif/esp32-camera` ^2.1 (pulls in `espressif/esp_jpeg`)
  - `espressif/led_strip` ^3.0
- The TLS root certificates are built into the firmware (ESP-IDF certificate bundle), so no certificate files are needed.

## Privacy behavior built into the firmware

- The mic is only streamed while the status LED is **blue**. Nothing is sent while idle.
- Photos are taken only when you hold the head or ask the parrot to look. Each photo plays a shutter click and lights the red privacy LED.
- The camera preview page (for testing) is served only on the local Wi-Fi, never to the internet. The privacy LED stays on while its live view is open. Turn it off in menuconfig before using the parrot on a shared network.
- The persona tells the model not to describe faces or guess who someone is.
- Audio and photos go to OpenAI under its API data policy. Check OpenAI's current API data-usage terms before demoing with real users.

## Later, not needed for the MVP

| Service | For |
|---|---|
| Token server (short-lived keys) | Keeping the real API key off devices other people hold |
| ElevenLabs Voice Design + Agents | A custom parrot voice (adds latency, see the voice discussion) |
| A relay server holding the WebSocket | Swapping models/providers without reflashing, logging lessons |
| OTA update hosting | Updating the firmware without a cable |
| Wi-Fi provisioning (phone app or SoftAP) | Setting the Wi-Fi without menuconfig |
