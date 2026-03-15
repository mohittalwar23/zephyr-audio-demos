/*
 * main_ml.c  –  I2S mic → ML keyword tap → delay → I2S sink
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include "backends/i2s_source.h"
#include "backends/i2s_sink.h"
#include "core/pcm_delay_node.h"
#include "core/pcm_convert.h"
#include "ml/pcm_ml_node.h"

LOG_MODULE_REGISTER(loopback_ml, LOG_LEVEL_INF);

#define SAMPLE_RATE    16000
#define NUM_CHANNELS   2
#define FRAMES         64
#define BLOCK_SIZE     (FRAMES * NUM_CHANNELS * sizeof(int16_t))
#define SLAB_COUNT     6
#define DELAY_BLOCKS   50      /* 50 × 4ms = 200ms — keeps heap small */
#define AUDIO_GAIN     4
#define LOG_INTERVAL   500

K_MEM_SLAB_DEFINE(rx_slab, BLOCK_SIZE, SLAB_COUNT, 4);
K_MEM_SLAB_DEFINE(tx_slab, BLOCK_SIZE, SLAB_COUNT, 4);

/* Static inference thread stack — avoids libc heap allocation */
PCM_ML_NODE_STACK_DEFINE(ml_infer_stack);

static i2s_source_t     rx_source;
static i2s_sink_t       tx_sink;
static pcm_delay_node_t delay_node;
static pcm_ml_node_t    ml_node;

static void on_ml_result(const char *label, float score, void *ctx)
{
    ARG_UNUSED(ctx);
    ARG_UNUSED(label);
    ARG_UNUSED(score);
}

static void log_stats(uint32_t n)
{
    if (n % LOG_INTERVAL != 0) return;
    LOG_INF("source ok=%u overruns=%u",
            rx_source.base.stats.blocks_ok,
            rx_source.base.stats.overruns);
    LOG_INF("delay  depth=%u dropped=%u",
            pcm_delay_node_stats(&delay_node)->queue_depth,
            pcm_delay_node_stats(&delay_node)->dropped);
    LOG_INF("sink   ok=%u dropped=%u restarts=%u",
            tx_sink.base.stats.blocks_ok,
            tx_sink.base.stats.dropped,
            tx_sink.base.stats.restarts);
    pcm_ml_node_log_stats(&ml_node);
}

int main(void)
{
    int err;

    pcm_stream_info_t fmt = {
        .sample_rate = SAMPLE_RATE,
        .channels    = NUM_CHANNELS,
        .format      = PCM_FMT_S16_LE,
        .frames      = FRAMES,
    };

    i2s_source_cfg_t src_cfg = {
        .dev               = DEVICE_DT_GET(DT_ALIAS(i2s_rx)),
        .slab              = &rx_slab,
        .stream            = fmt,
        .driver_timeout_ms = 1000,
    };
    err = i2s_source_init(&rx_source, &src_cfg);
    if (err < 0) { LOG_ERR("source init: %d", err); return err; }

    i2s_sink_cfg_t snk_cfg = {
        .dev               = DEVICE_DT_GET(DT_ALIAS(i2s_tx)),
        .slab              = &tx_slab,
        .stream            = fmt,
        .driver_timeout_ms = 2000,
        .prefill_blocks    = 3,
    };
    err = i2s_sink_init(&tx_sink, &snk_cfg);
    if (err < 0) { LOG_ERR("sink init: %d", err); return err; }

    /* ML node — pass static stack, no heap allocation */
    pcm_ml_node_cfg_t ml_cfg = {
        .input_channels = NUM_CHANNELS,
        .result_cb      = on_ml_result,
        .result_cb_ctx  = NULL,
        .infer_priority = 7,
    };
    err = pcm_ml_node_init(&ml_node, &ml_cfg, ml_infer_stack);
    if (err < 0) { LOG_ERR("ml_node init: %d", err); return err; }

    pcm_delay_node_cfg_t dly_cfg = {
        .delay_blocks = DELAY_BLOCKS,
        .max_blocks   = 0,
    };
    err = pcm_delay_node_init(&delay_node, &dly_cfg);
    if (err < 0) { LOG_ERR("delay init: %d", err); return err; }

    err = pcm_source_start((pcm_source_t *)&rx_source);
    if (err < 0) { LOG_ERR("source start: %d", err); return err; }

    LOG_INF("ML pipeline ready — listening for keywords");

    bool     tx_started  = false;
    uint32_t block_count = 0;

    while (1) {
        pcm_block_t raw = {0};
        err = pcm_source_read((pcm_source_t *)&rx_source, &raw, K_FOREVER);
        if (err < 0) { LOG_WRN("source read err %d", err); continue; }

        pcm_block_t stereo = {0};
        err = pcm_mono_to_stereo_s16(&raw, &stereo, AUDIO_GAIN);
        pcm_source_release((pcm_source_t *)&rx_source, &raw);
        if (err < 0) { LOG_WRN("convert err %d", err); continue; }

        /* ML tap — non-destructive */
        pcm_ml_node_process(&ml_node, &stereo);

        err = pcm_delay_node_push(&delay_node, &stereo);
        if (err < 0) { k_free(stereo.data); continue; }

        if (!tx_started && pcm_delay_node_is_primed(&delay_node)) {
            err = pcm_sink_start((pcm_sink_t *)&tx_sink);
            if (err < 0) { LOG_ERR("sink start: %d", err); return err; }
            tx_started = true;
            LOG_INF("TX started");
        }

        if (tx_started) {
            pcm_block_t delayed = {0};
            err = pcm_delay_node_pop(&delay_node, &delayed);
            if (err == -ENODATA) {
                pcm_sink_drain((pcm_sink_t *)&tx_sink);
                pcm_sink_start((pcm_sink_t *)&tx_sink);
                tx_started = false;
            } else if (err == 0) {
                err = pcm_sink_write((pcm_sink_t *)&tx_sink,
                                     &delayed, K_MSEC(500));
                if (err < 0) k_free(delayed.data);
            }
        }

        log_stats(++block_count);
    }

    return 0;
}
