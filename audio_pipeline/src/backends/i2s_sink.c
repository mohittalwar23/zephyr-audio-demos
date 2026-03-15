/*
 * i2s_sink.c  –  I2S TX backend for pcm_sink_t
 *
 * See i2s_sink.h for full ownership and error-handling contract.
 */

#include "backends/i2s_sink.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(i2s_sink, LOG_LEVEL_DBG);

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static i2s_sink_t *to_i2s_snk(pcm_sink_t *base)
{
    return (i2s_sink_t *)base;  /* base is first member – safe cast */
}

/**
 * Attempt a clean drain-then-restart sequence after a TX error.
 * We do NOT prefill silence here; the caller can decide if re-prefill
 * is needed (e.g. for a delay pipeline node that may have queued data).
 */
static void restart_tx(i2s_sink_t *sink)
{
    LOG_WRN("i2s_sink: restarting TX device");
    i2s_trigger(sink->cfg.dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
    i2s_trigger(sink->cfg.dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
    i2s_trigger(sink->cfg.dev, I2S_DIR_TX, I2S_TRIGGER_START);
    sink->base.stats.restarts++;
}

/* ------------------------------------------------------------------ */
/* vtable implementations                                              */
/* ------------------------------------------------------------------ */

/**
 * write() – copy caller's PCM block into a TX slab slot and hand it
 *           to the I2S driver.
 *
 * Ownership:
 *   On success  → sink has freed the caller's block (k_free(block->data));
 *                 caller must NOT touch it again.
 *   On failure  → block->data still valid; caller must release it.
 *
 * The copy into a slab slot is mandatory: the Zephyr I2S driver expects
 * DMA-accessible slab memory, not arbitrary heap pointers.
 */
static int i2s_sink_write_impl(pcm_sink_t  *base,
                               pcm_block_t *block,
                               k_timeout_t  timeout)
{
    i2s_sink_t *sink = to_i2s_snk(base);
    void *slab_buf = NULL;

    /* --- Allocate a slab slot for the driver ------------------------ */
    int err = k_mem_slab_alloc(sink->cfg.slab, &slab_buf, timeout);
    if (err < 0) {
        /*
         * TX slab full → driver is not consuming fast enough.
         * This is an overrun from the pipeline's perspective.
         * Caller retains ownership and must drop or buffer the block.
         */
        LOG_WRN("i2s_sink: TX slab full (overrun)");
        sink->base.stats.overruns++;
        return -ENOBUFS;
    }

    /* Validate the incoming block fits the slab slot */
    size_t expected = pcm_block_size(&sink->cfg.stream);
    if (block->size > expected) {
        LOG_WRN("i2s_sink: block size %zu > slab slot %zu — truncating",
                block->size, expected);
        block->size = expected;
    }

    /* Copy PCM data into DMA-accessible slab memory */
    memset(slab_buf, 0, expected);          /* zero-pad if block is short  */
    memcpy(slab_buf, block->data, block->size);

    /*
     * Ownership transfer: caller's heap block is consumed; free it now.
     * The slab slot is passed to the driver which frees it post-DMA.
     */
    k_free(block->data);
    block->data = NULL;

    /* --- Hand slab slot to the I2S driver --------------------------- */
    err = i2s_write(sink->cfg.dev, slab_buf, expected);
    if (err < 0) {
        LOG_ERR("i2s_sink: i2s_write failed: %d", err);
        k_mem_slab_free(sink->cfg.slab, slab_buf);
        sink->base.stats.dropped++;

        /* Auto-restart so the next write attempt works */
        restart_tx(sink);
        return err;
    }

    sink->base.stats.blocks_ok++;
    return 0;
}

/** start() – configure the device then trigger DMA start. */
static int i2s_sink_start_impl(pcm_sink_t *base)
{
    i2s_sink_t *sink = to_i2s_snk(base);

    if (sink->running) {
        LOG_WRN("i2s_sink: already running");
        return 0;
    }

    struct i2s_config cfg = {
        .word_size      = pcm_bytes_per_sample(sink->cfg.stream.format) * 8u,
        .channels       = sink->cfg.stream.channels,
        .format         = I2S_FMT_DATA_FORMAT_I2S,
        .options        = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
        .frame_clk_freq = sink->cfg.stream.sample_rate,
        .mem_slab       = sink->cfg.slab,
        .block_size     = pcm_block_size(&sink->cfg.stream),
        .timeout        = (int32_t)sink->cfg.driver_timeout_ms,
    };

    int err = i2s_configure(sink->cfg.dev, I2S_DIR_TX, &cfg);
    if (err < 0) {
        LOG_ERR("i2s_sink: configure failed: %d", err);
        return err;
    }

    /* Prefill silence to warm up the TX FIFO before starting */
    if (sink->cfg.prefill_blocks > 0) {
        err = i2s_sink_prefill_silence(sink, sink->cfg.prefill_blocks);
        if (err < 0) {
            LOG_WRN("i2s_sink: prefill failed: %d (continuing)", err);
        }
    }

    err = i2s_trigger(sink->cfg.dev, I2S_DIR_TX, I2S_TRIGGER_START);
    if (err < 0) {
        LOG_ERR("i2s_sink: trigger START failed: %d", err);
        return err;
    }

    sink->running = true;
    LOG_INF("i2s_sink: started (%u Hz, %u ch, %u frames/block, prefill=%u)",
            sink->cfg.stream.sample_rate,
            sink->cfg.stream.channels,
            sink->cfg.stream.frames,
            sink->cfg.prefill_blocks);
    return 0;
}

/** drain() – flush queued TX blocks then stop the device. */
static int i2s_sink_drain_impl(pcm_sink_t *base)
{
    i2s_sink_t *sink = to_i2s_snk(base);

    int err = i2s_trigger(sink->cfg.dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
    if (err < 0) {
        LOG_WRN("i2s_sink: DRAIN failed: %d", err);
    }
    sink->running = false;
    LOG_INF("i2s_sink: drained  (blocks_ok=%u overruns=%u dropped=%u restarts=%u)",
            sink->base.stats.blocks_ok,
            sink->base.stats.overruns,
            sink->base.stats.dropped,
            sink->base.stats.restarts);
    return err;
}

/** stop() – hard stop; discard any queued DMA blocks. */
static int i2s_sink_stop_impl(pcm_sink_t *base)
{
    i2s_sink_t *sink = to_i2s_snk(base);

    if (!sink->running) return 0;

    int err = i2s_trigger(sink->cfg.dev, I2S_DIR_TX, I2S_TRIGGER_STOP);
    if (err < 0) {
        LOG_WRN("i2s_sink: STOP trigger failed: %d", err);
    }
    i2s_trigger(sink->cfg.dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);

    sink->running = false;
    LOG_INF("i2s_sink: stopped");
    return err;
}

/* ------------------------------------------------------------------ */
/* Public helpers                                                      */
/* ------------------------------------------------------------------ */

int i2s_sink_prefill_silence(i2s_sink_t *sink, uint8_t n_blocks)
{
    size_t block_bytes = pcm_block_size(&sink->cfg.stream);

    for (uint8_t i = 0; i < n_blocks; i++) {
        void *buf = NULL;
        int err = k_mem_slab_alloc(sink->cfg.slab, &buf,
                                   K_MSEC(sink->cfg.driver_timeout_ms));
        if (err < 0) {
            LOG_ERR("i2s_sink: prefill slab alloc failed at block %u", i);
            return -ENOMEM;
        }
        memset(buf, 0, block_bytes);

        err = i2s_write(sink->cfg.dev, buf, block_bytes);
        if (err < 0) {
            LOG_ERR("i2s_sink: prefill i2s_write failed: %d", err);
            k_mem_slab_free(sink->cfg.slab, buf);
            return err;
        }
    }

    LOG_DBG("i2s_sink: prefilled %u silence block(s)", n_blocks);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public init                                                         */
/* ------------------------------------------------------------------ */

int i2s_sink_init(i2s_sink_t *sink, const i2s_sink_cfg_t *cfg)
{
    if (!sink || !cfg || !cfg->dev || !cfg->slab) {
        return -EINVAL;
    }

    if (!device_is_ready(cfg->dev)) {
        LOG_ERR("i2s_sink: device not ready");
        return -ENODEV;
    }

    memset(sink, 0, sizeof(*sink));

    /* Populate vtable */
    sink->base.write = i2s_sink_write_impl;
    sink->base.start = i2s_sink_start_impl;
    sink->base.drain = i2s_sink_drain_impl;
    sink->base.stop  = i2s_sink_stop_impl;
    sink->base.info  = cfg->stream;

    sink->cfg     = *cfg;
    sink->running = false;

    LOG_INF("i2s_sink: initialised on %s", cfg->dev->name);
    return 0;
}
