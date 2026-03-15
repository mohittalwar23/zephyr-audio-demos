# audio_pipeline — Real-Time PCM Audio Pipeline for Zephyr

A backend-agnostic real-time audio pipeline sample for Zephyr RTOS,
validated on ESP32 with an INMP441 I2S microphone and MAX98357A I2S
speaker amplifier. Demonstrates capture, buffering, format conversion,
configurable delay, and optional on-device keyword spotting via
TensorFlow Lite for Microcontrollers.

This work is being developed as part of a GSoC 2026 proposal for the
Zephyr project: *"Real-Time Audio Capture and Playback Pipeline for
Zephyr with Optional ML Integration"*.

Author: Mohit Talwar

## Pipeline Architecture

The pipeline is built from composable stages. Each stage communicates
only through `pcm_block_t` — a PCM data pointer plus metadata
(sample rate, channels, format, frame count). No stage knows what
hardware the adjacent stage uses.

```
                        LOOPBACK pipeline
  ┌─────────────┐     ┌───────────┐     ┌──────────────┐     ┌─────────────┐
  │ i2s_source  │────▶│pcm_convert│────▶│pcm_delay_node│────▶│  i2s_sink   │
  │ (INMP441)   │     │mono→stereo│     │  (N blocks)  │     │ (MAX98357A) │
  └─────────────┘     └───────────┘     └──────────────┘     └─────────────┘

                        ML pipeline
  ┌─────────────┐     ┌───────────┐     ┌─────────────┐     ┌──────────────┐     ┌─────────────┐
  │ i2s_source  │────▶│pcm_convert│────▶│ pcm_ml_node │────▶│pcm_delay_node│────▶│  i2s_sink   │
  │ (INMP441)   │     │mono→stereo│     │  (tap only) │     │  (N blocks)  │     │ (MAX98357A) │
  └─────────────┘     └───────────┘     └──────┬──────┘     └──────────────┘     └─────────────┘
                                                │ copy (non-destructive)
                                         ┌──────▼──────┐
                                         │  inference  │
                                         │   thread    │
                                         │ (TFLM MFSC  │
                                         │  + CNN)     │
                                         └─────────────┘
```

### Abstraction boundary

The **init section** of `main.c` is intentionally backend-specific, it wires concrete hardware to the abstract interface:

```c
i2s_source_cfg_t src_cfg = { .dev = DEVICE_DT_GET(DT_ALIAS(i2s_rx)), ... };
i2s_source_init(&rx_source, &src_cfg);
```

The **pipeline loop** is fully backend-agnostic , it only calls the
abstract interface and has no knowledge of I2S, DMIC, or any hardware:

```c
pcm_source_read((pcm_source_t *)&rx_source, &block, K_FOREVER);
pcm_delay_node_push(&delay_node, &block);
pcm_sink_write((pcm_sink_t *)&tx_sink, &block, K_MSEC(500));
```

This is the correct boundary. Adding a DMIC or USB backend requires
zero changes to the pipeline loop, delay node, ML node, or sink.


## Building

### Prerequisites

```bash
# Enable the optional west group (tflite-micro lives there)
west config manifest.group-filter +optional
west update tflite-micro
```

### Loopback (mic → delay → speaker, no ML)

```bash
west build --pristine -b esp32_devkitc/esp32/procpu . -- -DPIPELINE=loopback
west flash
```

### ML pipeline — INT8 preprocessor (default, fits ESP32)

```bash
west build --pristine -b esp32_devkitc/esp32/procpu .
west flash
```

### ML pipeline — float32 preprocessor

```bash
west build --pristine -b esp32_devkitc/esp32/procpu . -- -DMICRO_SPEECH_FLOAT_PREPROCESSOR=y
west flash
```

### Monitor serial output

```bash
west espressif monitor
```

## ML Models

Two-stage pipeline from the TensorFlow Lite micro_speech example:

| Stage | Model | Arena |
|---|---|---|
| Audio preprocessor (INT8) | MFSC feature extraction, INT8 quantized | 16 KB |
| Audio preprocessor (float32) | MFSC feature extraction, float32 | 32 KB |
| Keyword classifier | Depthwise-separable CNN, INT8 | 12 KB |

**Output labels:** `silence` / `unknown` / `yes` / `no`

The ML node is a **non-destructive tap** — the audio stream passes
through to the delay node and speaker unchanged. Inference runs in a
separate thread at lower priority so a slow inference run never causes
a TX underrun.

**Sliding window:** inference fires every 500 ms on the latest 1 second
of audio, keeping worst-case detection latency under ~2 seconds.


## Design Decisions

| Requirement | How it's addressed |
|---|---|
| Backend-agnostic pipeline loop | `pcm_block_t` + vtable; pipeline loop never calls I2S directly |
| Backend-specific init is intentional | `i2s_source_cfg_t` in `main.c` init — correct wiring layer, not a leak |
| Explicit buffer ownership | Documented on every API; rules enforced consistently across all stages |
| Channel/format conversion as separate stage | `pcm_convert.h` — not buried in backends |
| Rolling delay as pipeline node | `pcm_delay_node_t` — reusable, not app logic |
| ML as optional non-blocking tap | `pcm_ml_node_t` — sem-triggered inference thread; audio path unaffected |
| Backpressure / underrun / overrun | `pcm_stats_t` on every stage; `-ENOBUFS` / `-ENODATA` return codes |
| Observability | Stats logged every 500 blocks |
| Automatic Kconfig per variant | `EXTRA_CONF_FILE` set in CMakeLists before `find_package(Zephyr)` |


## Buffer Ownership Rules

```
pcm_source_read()        → caller owns block->data  (must call release())
pcm_source_release()     → frees block->data
pcm_sink_write()         → sink takes ownership on success
pcm_delay_node_push()    → node takes ownership on success
pcm_delay_node_pop()     → caller owns block->data (must k_free or pass to sink)
pcm_ml_node_process()    → non-destructive tap, caller retains ownership
pcm_mono_to_stereo_s16() → allocates new output, input ownership stays with caller
```


