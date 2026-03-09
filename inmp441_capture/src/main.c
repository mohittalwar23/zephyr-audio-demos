#include <zephyr/kernel.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(i2s_mic, LOG_LEVEL_DBG);

#define SAMPLE_RATE      16000
#define BUFFER_LEN       64
#define BYTES_PER_SAMPLE sizeof(int16_t)
#define BLOCK_SIZE       (BUFFER_LEN * BYTES_PER_SAMPLE)
#define BLOCK_COUNT      4

K_MEM_SLAB_DEFINE(rx_mem_slab, BLOCK_SIZE, BLOCK_COUNT, 4);

static const struct device *i2s_dev  = DEVICE_DT_GET(DT_NODELABEL(i2s0));
static const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));

/* Send raw bytes over UART — replaces printk for binary data */
static void uart_send_raw(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(uart_dev, data[i]);
    }
}

static int i2s_setup(void)
{
    if (!device_is_ready(i2s_dev)) {
        return -ENODEV;
    }

    struct i2s_config cfg = {
        .word_size      = 16,
        .channels       = 2,
        .format         = I2S_FMT_DATA_FORMAT_I2S,
        .options        = I2S_OPT_BIT_CLK_MASTER |
                          I2S_OPT_FRAME_CLK_MASTER,
        .frame_clk_freq = SAMPLE_RATE,
        .mem_slab       = &rx_mem_slab,
        .block_size     = BLOCK_SIZE,
        .timeout        = 1000,
    };

    int err = i2s_configure(i2s_dev, I2S_DIR_RX, &cfg);
    if (err < 0) {
        return err;
    }

    return i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);
}

int main(void)
{
    if (!device_is_ready(uart_dev)) {
        return -ENODEV;
    }

    if (i2s_setup() < 0) {
        return -1;
    }

    while (1) {
        void  *mem_block = NULL;
        size_t block_size = 0;

        int err = i2s_read(i2s_dev, &mem_block, &block_size);
        if (err < 0) {
            i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_PREPARE);
            i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);
            continue;
        }

        int16_t *samples     = (int16_t *)mem_block;
        int      num_samples = block_size / BYTES_PER_SAMPLE;

        /*
         * INMP441 is left-channel only.
         * Zephyr reads stereo interleaved [L, R, L, R ...].
         * Extract only left channel samples into a mono buffer
         * then send as raw 16-bit PCM over UART.
         */
        int16_t mono[BUFFER_LEN];
        int     mono_count = 0;

        for (int i = 0; i < num_samples; i += 2) {
            mono[mono_count++] = samples[i];  /* left channel only */
        }

        /* Send raw binary PCM — captured with: cat /dev/ttyUSB0 > rec.raw */
        uart_send_raw((uint8_t *)mono, mono_count * BYTES_PER_SAMPLE);

        k_mem_slab_free(&rx_mem_slab, mem_block);
    }

    return 0;
}