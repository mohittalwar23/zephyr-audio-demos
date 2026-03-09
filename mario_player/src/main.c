#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/device.h>
#include <math.h>

#define SAMPLE_RATE    22050
#define NUM_CHANNELS   2
/* BLOCK_SIZE = number of int16 samples in one DMA block (L+R pairs) */
#define BLOCK_SIZE     256
#define BLOCK_BYTES    (BLOCK_SIZE * sizeof(int16_t))
#define NUM_BLOCKS     6
#define VOLUME         8000

/* ── Note frequencies ── */
// #define E5  659.25f
// #define C5  523.25f
// #define G5  783.99f
// #define G4  392.00f
// #define A4  440.00f
// #define B4  493.88f
// #define AS4 466.16f
// #define A5  880.00f
// #define F5  698.46f
// #define E4  329.63f
// #define D4  293.66f
// #define REST 0.0f

struct note {
    float freq;
    int   dur_ms;
};

// static const struct note melody[] = {
//     {E5,150},{E5,150},{REST,75},{E5,150},{REST,75},{C5,150},{E5,150},
//     {G5,300},{REST,300},{G4,300},{REST,300},

//     {C5,150},{REST,150},{G4,150},{REST,300},{E4,150},
//     {REST,150},{A4,150},{REST,75},{B4,150},{REST,75},{AS4,150},{A4,150},
//     {G4,150},{E5,150},{G5,150},{A5,150},{REST,75},{F5,150},{G5,75},
//     {REST,75},{E5,150},{REST,75},{C5,150},{D4,150},{B4,150},{REST,150},

//     {C5,150},{REST,150},{G4,150},{REST,300},{E4,150},
//     {REST,150},{A4,150},{REST,75},{B4,150},{REST,75},{AS4,150},{A4,150},
//     {G4,150},{E5,150},{G5,150},{A5,150},{REST,75},{F5,150},{G5,75},
//     {REST,75},{E5,150},{REST,75},{C5,150},{D4,150},{B4,150},{REST,150},
// };

#define B4  493.88f
#define E5  659.25f
#define D5  587.33f
#define C5  523.25f
#define A4  440.00f
#define A5  880.00f
#define C4  261.63f
#define D4  293.66f
#define E4  329.63f
#define F4  349.23f
#define G4  392.00f
#define G5  783.99f
#define F5  698.46f
#define REST 0.0f

static const struct note melody[] = {
    /* Line 1 */
    {E5,300},{B4,150},{C5,150},{D5,300},{C5,150},{B4,150},
    {A4,300},{A4,150},{C5,150},{E5,300},{D5,150},{C5,150},
    {B4,450},{C5,150},{D5,300},{E5,300},
    {C5,300},{A4,300},{A4,300},{REST,150},

    /* Line 2 */
    {REST,150},{D5,300},{F5,150},{A5,300},{G5,150},{F5,150},
    {E5,450},{C5,150},{E5,300},{D5,150},{C5,150},
    {B4,300},{B4,150},{C5,150},{D5,300},{E5,300},
    {C5,300},{A4,300},{A4,300},{REST,300},

    /* Line 3 - faster run */
    {E4,150},{E4,150},{C4,150},{D4,150},{E4,150},{F4,150},{G4,150},{E4,150},
    {A4,300},{REST,150},{A4,150},{A4,150},{G4,150},{F4,150},
    {E4,150},{F4,150},{G4,150},{A4,150},{B4,150},{C5,150},{D5,150},{B4,150},
    {C5,300},{REST,300},
};
K_MEM_SLAB_DEFINE(tx_slab, BLOCK_BYTES, NUM_BLOCKS, 4);

/* global sample counter so phase is continuous across blocks */
static uint32_t g_sample_pos = 0;

/*
 * Fill one DMA block with stereo sine samples.
 * frames = BLOCK_SIZE / 2  (each frame = L sample + R sample)
 */
static void fill_block(int16_t *buf, float freq)
{
    int frames = BLOCK_SIZE / NUM_CHANNELS;  /* 128 frames */

    for (int i = 0; i < frames; i++) {
        int16_t sample = 0;
        if (freq != REST) {
            float t = (float)(g_sample_pos + i) / SAMPLE_RATE;
            sample = (int16_t)(VOLUME * sinf(2.0f * (float)M_PI * freq * t));
        }
        buf[i * 2]     = sample;  /* Left  */
        buf[i * 2 + 1] = sample;  /* Right */
    }
    g_sample_pos += frames;
}

static int write_block(const struct device *dev, float freq)
{
    void *mem_block;

    if (k_mem_slab_alloc(&tx_slab, &mem_block, K_MSEC(500)) < 0) {
        printk("slab alloc failed\n");
        return -ENOMEM;
    }

    fill_block((int16_t *)mem_block, freq);

    int ret = i2s_write(dev, mem_block, BLOCK_BYTES);
    if (ret < 0) {
        printk("i2s_write err: %d\n", ret);
        k_mem_slab_free(&tx_slab, mem_block);
        return ret;
    }
    return 0;
}

static void play_melody(const struct device *dev)
{
    int notes = ARRAY_SIZE(melody);
    bool started = false;

    for (int n = 0; n < notes; n++) {
        float freq   = melody[n].freq;
        int   dur_ms = melody[n].dur_ms;

        /* frames needed for this note duration */
        int frames_needed = (SAMPLE_RATE * dur_ms) / 1000;
        int frames_done   = 0;
        int frames_per_block = BLOCK_SIZE / NUM_CHANNELS;

        while (frames_done < frames_needed) {
            if (write_block(dev, freq) < 0) {
                return;
            }
            frames_done += frames_per_block;

            /* Start playback after pre-filling 2 blocks */
            if (!started && frames_done >= frames_per_block * 2) {
                i2s_trigger(dev, I2S_DIR_TX, I2S_TRIGGER_START);
                started = true;
            }
        }
    }

    /* drain remaining DMA buffers */
    i2s_trigger(dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
}

int main(void)
{
    const struct device *i2s_dev = DEVICE_DT_GET(DT_ALIAS(i2s_tx));

    if (!device_is_ready(i2s_dev)) {
        printk("I2S device not ready!\n");
        return -1;
    }

    struct i2s_config cfg = {
        .word_size      = 16,
        .channels       = NUM_CHANNELS,
        .format         = I2S_FMT_DATA_FORMAT_I2S,
        .options        = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
        .frame_clk_freq = SAMPLE_RATE,
        .mem_slab       = &tx_slab,
        .block_size     = BLOCK_BYTES,
        .timeout        = 2000,
    };

    int ret = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
    if (ret < 0) {
        printk("i2s_configure failed: %d\n", ret);
        return ret;
    }

    printk("Mario time! Let's-a go!\n");

    while (1) {
        g_sample_pos = 0;  /* reset phase each loop */
        play_melody(i2s_dev);
        k_sleep(K_MSEC(1000));
    }

    return 0;
}