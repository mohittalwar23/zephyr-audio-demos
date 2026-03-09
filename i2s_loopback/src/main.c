#include <zephyr/kernel.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(i2s_loopback, LOG_LEVEL_DBG);

#define SAMPLE_RATE       16000
#define NUM_CHANNELS      2
#define BUFFER_LEN        64
#define BLOCK_SIZE        (BUFFER_LEN * NUM_CHANNELS * sizeof(int16_t))  /* 256 bytes */
#define SLAB_COUNT        6

/*
 * DELAY QUEUE
 * -----------
 * We record into a queue of heap-allocated nodes.
 * Playback starts only after DELAY_BLOCKS have been queued.
 * After that, every new RX block enqueues at the tail and
 * one block dequeues from the head to TX — a rolling delay.
 *
 * Memory cost = DELAY_BLOCKS * BLOCK_SIZE (heap, not BSS)
 *
 * At 16kHz, BUFFER_LEN=64:
 *   MS_PER_BLOCK = (64*1000)/16000 = 4 ms per block
 *   DELAY_BLOCKS = 500 → 2 seconds   (500 * 256 = 125 KB heap)
 *   DELAY_BLOCKS = 250 → 1 second    (250 * 256 =  62 KB heap)
 *   DELAY_BLOCKS = 125 → 0.5 seconds (125 * 256 =  31 KB heap)
 *
 * Tune DELAY_BLOCKS + CONFIG_HEAP_MEM_POOL_SIZE together.
 */
 
#define DELAY_BLOCKS      250          /* 1 second delay */
#define TX_PREFILL        3

K_MEM_SLAB_DEFINE(rx_slab, BLOCK_SIZE, SLAB_COUNT, 4);
K_MEM_SLAB_DEFINE(tx_slab, BLOCK_SIZE, SLAB_COUNT, 4);

static const struct device *i2s_rx_dev = DEVICE_DT_GET(DT_ALIAS(i2s_rx));
static const struct device *i2s_tx_dev = DEVICE_DT_GET(DT_ALIAS(i2s_tx));

/* ------------------------------------------------------------------ */
/* Singly-linked queue node — holds one block of audio                 */
/* ------------------------------------------------------------------ */
struct audio_node {
    struct audio_node *next;
    int16_t            data[BUFFER_LEN * NUM_CHANNELS];
};

/* Simple FIFO queue — head = next to play, tail = last recorded */
static struct audio_node *q_head = NULL;
static struct audio_node *q_tail = NULL;
static int                q_len  = 0;

static void q_push(struct audio_node *node)
{
    node->next = NULL;
    if (q_tail) q_tail->next = node;
    else        q_head = node;
    q_tail = node;
    q_len++;
}

static struct audio_node *q_pop(void)
{
    if (!q_head) return NULL;
    struct audio_node *node = q_head;
    q_head = node->next;
    if (!q_head) q_tail = NULL;
    q_len--;
    return node;
}

static void q_free_all(void)
{
    struct audio_node *n;
    while ((n = q_pop()) != NULL) k_free(n);
}

/* ------------------------------------------------------------------ */
static int configure_rx(void)
{
    struct i2s_config cfg = {
        .word_size      = 16,
        .channels       = NUM_CHANNELS,
        .format         = I2S_FMT_DATA_FORMAT_I2S,
        .options        = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
        .frame_clk_freq = SAMPLE_RATE,
        .mem_slab       = &rx_slab,
        .block_size     = BLOCK_SIZE,
        .timeout        = 1000,
    };
    int err = i2s_configure(i2s_rx_dev, I2S_DIR_RX, &cfg);
    if (err < 0) LOG_ERR("RX configure failed: %d", err);
    return err;
}

static int configure_tx(void)
{
    struct i2s_config cfg = {
        .word_size      = 16,
        .channels       = NUM_CHANNELS,
        .format         = I2S_FMT_DATA_FORMAT_I2S,
        .options        = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
        .frame_clk_freq = SAMPLE_RATE,
        .mem_slab       = &tx_slab,
        .block_size     = BLOCK_SIZE,
        .timeout        = 2000,
    };
    int err = i2s_configure(i2s_tx_dev, I2S_DIR_TX, &cfg);
    if (err < 0) LOG_ERR("TX configure failed: %d", err);
    return err;
}

static int prefill_silence(void)
{
    for (int i = 0; i < TX_PREFILL; i++) {
        void *buf;
        if (k_mem_slab_alloc(&tx_slab, &buf, K_MSEC(200)) < 0) {
            LOG_ERR("tx slab alloc failed (prefill %d)", i);
            return -ENOMEM;
        }
        memset(buf, 0, BLOCK_SIZE);
        int err = i2s_write(i2s_tx_dev, buf, BLOCK_SIZE);
        if (err < 0) {
            k_mem_slab_free(&tx_slab, buf);
            return err;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Write one audio_node out to TX i2s                                  */
/* ------------------------------------------------------------------ */
static int play_node(struct audio_node *node)
{
    void *tx_block = NULL;
    if (k_mem_slab_alloc(&tx_slab, &tx_block, K_MSEC(500)) < 0) {
        LOG_WRN("TX slab full — dropping");
        return -ENOMEM;
    }

    /* Expand INMP441 mono left → both stereo channels */
    int16_t *src = node->data;
    int16_t *dst = (int16_t *)tx_block;
    for (int i = 0; i < BUFFER_LEN; i++) {
        int16_t s      = src[i * 2];
        dst[i * 2]     = s;
        dst[i * 2 + 1] = s;
    }

    int err = i2s_write(i2s_tx_dev, tx_block, BLOCK_SIZE);
    if (err < 0) {
        LOG_WRN("i2s_write err: %d", err);
        k_mem_slab_free(&tx_slab, tx_block);
        return err;
    }
    return 0;  /* driver owns tx_block now */
}

/* ------------------------------------------------------------------ */
int main(void)
{
    if (!device_is_ready(i2s_rx_dev)) { LOG_ERR("RX not ready"); return -ENODEV; }
    if (!device_is_ready(i2s_tx_dev)) { LOG_ERR("TX not ready"); return -ENODEV; }

    if (configure_rx() < 0) return -1;
    if (configure_tx() < 0) return -1;

    LOG_INF("Delay loopback | DELAY_BLOCKS=%d | ~%d ms | heap needed ~%d KB",
            DELAY_BLOCKS,
            (DELAY_BLOCKS * BUFFER_LEN * 1000) / SAMPLE_RATE,
            (DELAY_BLOCKS * sizeof(struct audio_node)) / 1024);

    bool tx_started = false;

    /* Start RX immediately */
    i2s_trigger(i2s_rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);

    while (1) {
        /* ── 1. Read from mic ── */
        void  *rx_block = NULL;
        size_t rx_bytes = 0;

        int err = i2s_read(i2s_rx_dev, &rx_block, &rx_bytes);
        if (err < 0) {
            LOG_WRN("i2s_read err %d", err);
            i2s_trigger(i2s_rx_dev, I2S_DIR_RX, I2S_TRIGGER_PREPARE);
            i2s_trigger(i2s_rx_dev, I2S_DIR_RX, I2S_TRIGGER_START);
            tx_started = false;
            q_free_all();
            continue;
        }

        /* ── 2. Alloc a queue node and copy mic data into it ── */
        struct audio_node *node = k_malloc(sizeof(struct audio_node));
        if (!node) {
            /*
             * Heap full — oldest block falls off the queue to make room.
             * This keeps the delay window rolling even under memory pressure.
             */
            LOG_WRN("heap full — evicting oldest block");
            struct audio_node *old = q_pop();
            if (old) k_free(old);
            node = k_malloc(sizeof(struct audio_node));
            if (!node) {
                LOG_ERR("still no heap after evict — skipping block");
                k_mem_slab_free(&rx_slab, rx_block);
                continue;
            }
        }

        memcpy(node->data, rx_block, BLOCK_SIZE);
        k_mem_slab_free(&rx_slab, rx_block);

        q_push(node);

        /* ── 3. Once delay buffer is full, start TX and keep it rolling ── */
        if (!tx_started && q_len >= DELAY_BLOCKS) {
            LOG_INF("Delay buffer full (%d blocks) — starting playback", q_len);
            prefill_silence();
            i2s_trigger(i2s_tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
            tx_started = true;
        }

        /* ── 4. For every block recorded, play one from the head ── */
        if (tx_started) {
            struct audio_node *play = q_pop();
            if (play) {
                err = play_node(play);
                k_free(play);

                if (err < 0) {
                    /* TX error — drain and restart */
                    i2s_trigger(i2s_tx_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
                    prefill_silence();
                    i2s_trigger(i2s_tx_dev, I2S_DIR_TX, I2S_TRIGGER_START);
                }
            }
        }

        /* ── 5. Periodic status ── */
        static int dbg = 0;
        if (++dbg % 500 == 0) {
            LOG_INF("queue depth: %d blocks (~%d ms)",
                    q_len,
                    (q_len * BUFFER_LEN * 1000) / SAMPLE_RATE);
        }
    }

    return 0;
}