# Calios parrot: MVP firmware

ESP-IDF firmware for the stage-1 Calios parrot: a shoulder parrot you talk to in the language you're learning. It runs on a **Seeed XIAO ESP32S3 Sense** and uses the **OpenAI Realtime API** for listening, replying in a natural voice, and looking at photos.

- **Wiring:** [docs/WIRING.md](docs/WIRING.md) (pin map, diagram, power)
- **APIs and services:** [docs/SERVICES.md](docs/SERVICES.md)

## What it does

| You do | The parrot does |
|---|---|
| Tap its head | Chirps, LED turns blue, listens |
| Talk, then pause | Knows you finished (amber LED), replies out loud, beak moving with its voice (green LED) |
| Keep talking after it replies | It listens again for ~6 s, so you can have a back-and-forth without tapping each time |
| Tap while it's talking | Stops and listens to you |
| Tap while it's listening | Stops listening |
| Hold its head ~1.2 s | Takes a photo (shutter click, red privacy LED) and teaches you the word for what it sees |
| Ask "what's this called?" | The model decides to take a photo itself |

**Status LED:** purple blink = finding Wi-Fi · purple breathing = connecting · dim teal = ready · blue = listening · amber = thinking · green = talking · red blinks = error.

The head tilts while listening, sways while thinking, and bobs while talking.

## Setup

Works with **ESP-IDF v5.5.x** (built and checked with v5.5.3 and v5.5.6). It won't build on 6.x yet.

### In VS Code (ESP-IDF extension)

1. Open the `calios-fw` folder. The status bar should show **esp32s3** as the target; if not, run **ESP-IDF: Set Espressif Device Target** → esp32s3 → "ESP32-S3 chip (via builtin USB-JTAG)".
2. Run **ESP-IDF: SDK Configuration Editor (menuconfig)** and open the **Calios parrot** section.
3. Click **Build** (the wrench), then **Flash** (lightning bolt, UART), then **Monitor** (screen icon). Or **Build, Flash and Monitor** (flame) for all three.

The first build downloads three components (websocket client, camera, LED strip) into `managed_components/`, so it needs internet and takes a few minutes.

### From a terminal

```bash
idf.py set-target esp32s3
idf.py menuconfig                # -> "Calios parrot"
idf.py build flash monitor       # Ctrl+] to quit the monitor
```

### Settings to fill in (menuconfig → Calios parrot)

- **Wi-Fi** → network name and password (2.4 GHz; a phone hotspot works).
- **Voice service** → **OpenAI API key** (from a project with a budget limit).
- **Personality** → the language being learned, native language, level.

These are saved in `sdkconfig`, which `.gitignore` already excludes. Keep it that way, since your API key ends up in that file.

If the board isn't detected, hold the **B** (boot) button on the XIAO while plugging in the USB cable, then flash again.

When it's connected you'll hear a two-note chirp and the LED goes dim teal. The log shows what the parrot says, prefixed with its name.

## Tuning (all in menuconfig → Calios parrot)

| Problem | Setting |
|---|---|
| Too quiet / too loud / distorted | Audio → Speaker volume (default 50%) |
| It doesn't hear you, or cuts you off | Audio → Microphone gain (default 6×) |
| Touch doesn't register through the shell | Head input → turn on **Print touch readings**, then lower **Touch threshold** to about half the rise you see |
| Touch is flaky | Head input → **Push button**, and wire a button from D0 to GND |
| Beak doesn't close fully / opens too far | Servos → beak closed / open angles |
| Head isn't straight | Servos → head straight angle |
| Beak droops when idle | Servos → turn off **Switch servos off when still** |
| Cuts in while you pause to think | Talking → keep **Semantic** detection (default) |
| Noisy place, you'd rather control it | Talking → **Hold the head while talking** |
| Photo upside down / mirrored | Camera → flip / mirror |
| Want it to stop listening after each reply | Talking → follow-up time = 0 |

To change how the parrot teaches (correction style, how much native language, etc.), edit the text in `main/persona.c`.

## Code map

| File | What's in it |
|---|---|
| `main/board.h` | Every pin. Change wiring here. |
| `main/app.c` | The state machine: what each tap does, listening, timeouts, photos |
| `main/realtime.c` | The OpenAI Realtime WebSocket: session setup, streaming, replies, interrupting, the photo tool |
| `main/audio.c` | Mic capture (PDM), speaker playback (I2S), chirps, loudness for the beak |
| `main/servos.c` | Beak and head motion |
| `main/leds.c` | Status and privacy LED patterns |
| `main/input.c` | Touch pad / button |
| `main/camera.c` | Photo capture |
| `main/persona.c` | The parrot's personality and teaching rules |
| `main/auth.c` | Reads the API key from menuconfig |
| `main/wifi.c` | Wi-Fi |
| `main/Kconfig.projbuild` | All the menuconfig settings |

## Status

- Compiles cleanly with ESP-IDF v5.5.3 and v5.5.6 for the ESP32-S3 in three configurations: default; hold-to-talk + button + no camera; and silence-based detection + touch debug + small photos.
- **Not yet run on hardware.** The parts most likely to need adjusting on the first run are mic gain, the touch threshold, servo angles and speaker volume. All four are menuconfig settings, so no code changes should be needed.
- Not handled yet: trimming the conversation history when you interrupt (the model may assume you heard its whole reply), Wi-Fi setup without menuconfig, over-the-air updates, and battery operation.
