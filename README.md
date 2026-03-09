# Zephyr Audio Demos — ESP32

Three standalone Zephyr RTOS applications demonstrating I2S audio
capture and playback on an ESP32 DevKitC with an INMP441 microphone
and MAX98357A amplifier.

---

## Projects

### 1. `inmp441_capture` — I2S Microphone Capture
Captures audio from an INMP441 MEMS microphone over I2S and streams
raw 16-bit PCM frames over UART at 921600 baud.

On the host, record to a WAV file:
```bash
# Record 5 seconds
cat /dev/ttyUSB0 > rec.raw
# Convert to WAV (16kHz, mono, 16-bit)
sox -r 16000 -e signed -b 16 -c 1 rec.raw output.wav
```

**Wiring:**
| INMP441 | ESP32 GPIO |
|---------|-----------|
| SCK     | GPIO2     |
| WS      | GPIO15    |
| SD      | GPIO13    |
| L/R     | GND       |
| VDD     | 3.3V      |

---

### 2. `mario_player` — I2S Tone Sequencer / Mario Theme
Plays the Super Mario Bros and Tetris themes through a MAX98357A
amplifier using a sine-wave tone sequencer over I2S.

**Wiring:**
| MAX98357A | ESP32 GPIO |
|-----------|-----------|
| BCLK      | GPIO26    |
| LRC       | GPIO25    |
| DIN       | GPIO22    |
| VIN       | 5V        |

---

### 3. `i2s_loopback` — Record + Delayed Playback
Captures audio from the INMP441 mic and plays it back through the
MAX98357A speaker with a configurable rolling delay (~1 second).
Uses a heap-allocated FIFO queue to avoid large static buffers.

**Wiring:** Both mic and speaker connected simultaneously (see above).

---

## Building any project
```bash
cd ~/zephyrproject
source zephyr/zephyr-env.sh

# Example — build loopback
west build -b esp32_devkitc/esp32/procpu ~/path/to/zephyr-audio-demos/i2s_loopback

west flash
west espressif monitor
```

## Hardware

- **MCU:** ESP32 DevKitC (WROOM, no PSRAM)
- **Microphone:** INMP441 MEMS I2S microphone
- **Amplifier:** MAX98357A I2S DAC + amp
- **Zephyr version:** v4.3.0

