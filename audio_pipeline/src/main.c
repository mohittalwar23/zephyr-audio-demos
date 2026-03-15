/*
 * main.c  –  I2S delay loopback using the PCM pipeline abstraction
 *
 * Pipeline:
 *
 *   [INMP441 mic]
 *       │  I2S RX
 *   i2s_source_t
 *       │ pcm_block_t (S16_LE, 2ch*, 64 frames)
 *   pcm_convert: mono→stereo + gain       ← separate reusable stage
 *       │ pcm_block_t (S16_LE, 2ch, 64 frames)
 *   pcm_delay_node_t  (DELAY_BLOCKS deep) ← pipeline node, not app logic
 *       │ pcm_block_t (delayed)
 *   i2s_sink_t
 *       │  I2S TX
 *   [MAX98357A speaker]
 *
 * * The INMP441 is clocked as 2-channel but only fills the LEFT channel;
 *   the conversion stage handles the mono→stereo expansion with gain.
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include "backends/i2s_source.h"
#include "backends/i2s_sink.h"
#include "core/pcm_delay_node.h"
#include "core/pcm_convert.h"

LOG_MODULE_REGISTER(loopback, LOG_LEVEL_INF);

#define SAMPLE_RATE    16000
#define NUM_CHANNELS   2
#define FRAMES         64
#define BLOCK_SIZE     (FRAMES * NUM_CHANNELS * sizeof(int16_t))  /* 256 bytes */
#define SLAB_COUNT     6
#define DELAY_BLOCKS   250

#define AUDIO_GAIN     4    /* 1=unity, 8=very loud — tune to taste */


K_MEM_SLAB_DEFINE(rx_slab, BLOCK_SIZE, SLAB_COUNT, 4);
K_MEM_SLAB_DEFINE(tx_slab, BLOCK_SIZE, SLAB_COUNT, 4);

static i2s_source_t    rx_source;
static i2s_sink_t      tx_sink;
static pcm_delay_node_t delay_node;

#define LOG_INTERVAL 500

static void log_stats(uint32_t block_count)
{
    if (block_count % LOG_INTERVAL != 0) return;

    const pcm_stats_t *src  = &rx_source.base.stats;
    const pcm_stats_t *snk  = &tx_sink.base.stats;
    const pcm_stats_t *dly  = pcm_delay_node_stats(&delay_node);

    LOG_INF("--- pipeline stats (block %u) ---", block_count);
    LOG_INF("  source : ok=%u  overruns=%u  restarts=%u",
            src->blocks_ok, src->overruns, src->restarts);
    LOG_INF("  delay  : ok=%u  depth=%u  dropped=%u  underruns=%u",
            dly->blocks_ok, dly->queue_depth, dly->dropped, dly->underruns);
    LOG_INF("  sink   : ok=%u  overruns=%u  dropped=%u  restarts=%u",
            snk->blocks_ok, snk->overruns, snk->dropped, snk->restarts);
}


int main(void)
{
    int err;

    /* ── 1. Stream format ────────────────────────────────────────── */
    pcm_stream_info_t fmt = {
        .sample_rate = SAMPLE_RATE,
        .channels    = NUM_CHANNELS,
        .format      = PCM_FMT_S16_LE,
        .frames      = FRAMES,
    };

    /* ── 2. Init source ──────────────────────────────────────────── */
    i2s_source_cfg_t src_cfg = {
        .dev               = DEVICE_DT_GET(DT_ALIAS(i2s_rx)),
        .slab              = &rx_slab,
        .stream            = fmt,
        .driver_timeout_ms = 1000,
    };
    err = i2s_source_init(&rx_source, &src_cfg);
    if (err < 0) { LOG_ERR("source init: %d", err); return err; }

    /* ── 3. Init sink ────────────────────────────────────────────── */
    i2s_sink_cfg_t snk_cfg = {
        .dev               = DEVICE_DT_GET(DT_ALIAS(i2s_tx)),
        .slab              = &tx_slab,
        .stream            = fmt,
        .driver_timeout_ms = 2000,
        .prefill_blocks    = 3,
    };
    err = i2s_sink_init(&tx_sink, &snk_cfg);
    if (err < 0) { LOG_ERR("sink init: %d", err); return err; }

    /* ── 4. Init delay node ──────────────────────────────────────── */
    pcm_delay_node_cfg_t dly_cfg = {
        .delay_blocks = DELAY_BLOCKS,
        .max_blocks   = 0,  /* auto: delay + 10 */
    };
    err = pcm_delay_node_init(&delay_node, &dly_cfg);
    if (err < 0) { LOG_ERR("delay init: %d", err); return err; }

    /* ── 5. Start source ─────────────────────────────────────────── */
    err = pcm_source_start((pcm_source_t *)&rx_source);
    if (err < 0) { LOG_ERR("source start: %d", err); return err; }

    LOG_INF("pipeline ready — accumulating %u-block delay (~%u ms)…",
            DELAY_BLOCKS,
            (DELAY_BLOCKS * FRAMES * 1000U) / SAMPLE_RATE);

    /* ── 6. Pipeline loop ────────────────────────────────────────── */
    bool     tx_started  = false;
    uint32_t block_count = 0;

    while (1) {
        /* --- A. Read one block from the mic source ----------------- */
        pcm_block_t raw = {0};
        err = pcm_source_read((pcm_source_t *)&rx_source, &raw, K_FOREVER);
        if (err < 0) {
            LOG_WRN("source read err %d — retrying", err);
            continue;
        }

        /* --- B. Convert: INMP441 mono-left → stereo + gain -------- */
        pcm_block_t stereo = {0};
        err = pcm_mono_to_stereo_s16(&raw, &stereo, AUDIO_GAIN);
       
        pcm_source_release((pcm_source_t *)&rx_source, &raw);

        if (err < 0) {
            LOG_WRN("convert err %d — dropping block", err);
            continue;
        }

        /* --- C. Push converted block into the delay FIFO ---------- */
        err = pcm_delay_node_push(&delay_node, &stereo);
        if (err < 0) {
            /* push failed; stereo.data still ours — free it */
            k_free(stereo.data);
            LOG_WRN("delay push err %d", err);
            continue;
        }

        /* --- D. Start TX once the delay buffer is primed ---------- */
        if (!tx_started && pcm_delay_node_is_primed(&delay_node)) {
            err = pcm_sink_start((pcm_sink_t *)&tx_sink);
            if (err < 0) {
                LOG_ERR("sink start: %d", err);
                return err;
            }
            tx_started = true;
            LOG_INF("TX started — delay buffer full");
        }

        /* --- E. Pop one delayed block and play it ----------------- */
        if (tx_started) {
            pcm_block_t delayed = {0};
            err = pcm_delay_node_pop(&delay_node, &delayed);

            if (err == -EAGAIN) {
                /* Not primed yet — shouldn't happen post-start, but safe */
            } else if (err == -ENODATA) {
                /*
                 * Underrun: delay node ran dry.
                 * Restart the sink to avoid a TX stall.
                 */
                LOG_WRN("delay underrun — restarting sink");
                pcm_sink_drain((pcm_sink_t *)&tx_sink);
                pcm_sink_start((pcm_sink_t *)&tx_sink);
                tx_started = false;
            } else if (err == 0) {
                /*
                 * pcm_sink_write() takes ownership of delayed.data.
                 * On error, delayed.data is still ours — free it.
                 */
                err = pcm_sink_write((pcm_sink_t *)&tx_sink,
                                     &delayed, K_MSEC(500));
                if (err < 0) {
                    k_free(delayed.data);
                    LOG_WRN("sink write err %d", err);
                }
            }
        }

        /* --- F. Periodic stats ------------------------------------ */
        log_stats(++block_count);
    }

    /* Never reached in normal operation */
    pcm_source_stop((pcm_source_t *)&rx_source);
    pcm_sink_drain((pcm_sink_t *)&tx_sink);
    pcm_delay_node_flush(&delay_node);
    return 0;
}
