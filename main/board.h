// Calios parrot MVP: pin map for the Seeed XIAO ESP32S3 Sense.
// Full wiring guide: docs/WIRING.md
#pragma once

#include "driver/gpio.h"

// ---- Header pins (wired on the breadboard) --------------------------------
#define PIN_TOUCH        GPIO_NUM_1    // D0: copper-tape touch pad (or button to GND)
#define TOUCH_CHANNEL    1             // GPIO1 is touch channel 1 on the ESP32-S3
#define PIN_NEOPIXEL     GPIO_NUM_2    // D1: NeoPixel chain DIN
#define PIN_SERVO_BEAK   GPIO_NUM_3    // D2: beak servo signal (orange wire)
#define PIN_SERVO_HEAD   GPIO_NUM_4    // D3: head-tilt servo signal (orange wire)
#define PIN_I2S_BCLK     GPIO_NUM_5    // D4: MAX98357A BCLK
#define PIN_I2S_LRCLK    GPIO_NUM_6    // D5: MAX98357A LRC
#define PIN_I2S_DOUT     GPIO_NUM_43   // D6: MAX98357A DIN
#define PIN_AMP_SD       GPIO_NUM_44   // D7: MAX98357A SD (low = amp off)
// D8, D9, D10 (GPIO7/8/9) are free, but shared with the Sense board's microSD
// slot. Keep the slot empty if you use them.

// ---- On the Sense expansion board (no wiring needed) ----------------------
#define PIN_PDM_CLK      GPIO_NUM_42   // onboard PDM microphone clock
#define PIN_PDM_DATA     GPIO_NUM_41   // onboard PDM microphone data

#define CAM_PIN_XCLK     10
#define CAM_PIN_SIOD     40
#define CAM_PIN_SIOC     39
#define CAM_PIN_D7       48            // Y9
#define CAM_PIN_D6       11            // Y8
#define CAM_PIN_D5       12            // Y7
#define CAM_PIN_D4       14            // Y6
#define CAM_PIN_D3       16            // Y5
#define CAM_PIN_D2       18            // Y4
#define CAM_PIN_D1       17            // Y3
#define CAM_PIN_D0       15            // Y2
#define CAM_PIN_VSYNC    38
#define CAM_PIN_HREF     47
#define CAM_PIN_PCLK     13

// ---- LEDs -------------------------------------------------------------------
#define NUM_PIXELS       2
#define PIXEL_STATUS     0             // first NeoPixel in the chain: status
#define PIXEL_PRIVACY    1             // second NeoPixel: lit while the camera is used

// ---- Audio --------------------------------------------------------------------
// The OpenAI Realtime API takes and returns 24 kHz, 16-bit, mono PCM.
#define AUDIO_SAMPLE_RATE 24000
