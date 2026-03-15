/*
 * i2s_source.c  –  I2S RX backend for pcm_source_t
 *
 * See i2s_source.h for full ownership and error-handling contract.
 */

#include "backends/i2s_source.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(i2s_source, LOG_LEVEL_DBG);

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static i2s_source_t *to_i2s_src(pcm_source_t *base)
{
    /* Safe: i2s_source_t::base is the first member */
    return (i2s_source_t *)base;
}

/**
 * Push the RX I2S device back to a running state after an error.
 * PREPARE clears the error latch; START re-enables DMA.
 */
static void restart_rx(i2s_source_t *src)
{
    LOG_WRN("i2s_source: restarting RX device");
    i2s_trigger(src->cfg.dev, I2S_DIR_RX, I2S_TRIGGER_PREPARE);
    i2s_trigger(src->cfg.dev, I2S_DIR_RX, I2S_TRIGGER_START);
    src->base.stats.restarts++;
}

/* ------------------------------------------------------------------ */
/* vtable implementations                                              */
/* ------------------------------------------------------------------ */

/**
 * read() – pull one DMA block from the I2S driver, copy into a
 *          heap-allocated pcm_block_t, release the slab slot immediately.
 *
 * Ownership after return:
 *   Success  → caller owns *out_block (heap); must call release().
 *   Failure  → nothing allocated; caller does nothing.
 */
static int i2s_source_read_impl(pcm_source_t *base,
                                pcm_block_t  *out_block,
                                k_timeout_t   timeout)
{
    i2s_source_t *src = to_i2s_src(base);
    void  *drv_buf = NULL;
    size_t drv_bytes = 0;

    /* --- Pull from driver slab ---------------------------------------- */
    int err = i2s_read(src->cfg.dev, &drv_buf, &drv_bytes);
    if (err == -EIO) {
        /*
         * I2S driver signals a hardware error (e.g. DMA underrun on RX).
         * Restart and report so the caller can decide to retry.
         */
        src->base.stats.overruns++;
        restart_rx(src);
        return -EIO;
    }
    if (err < 0) {
        LOG_ERR("i2s_read failed: %d", err);
        src->base.stats.overruns++;
        return err;
    }

    /*
     * We got a slab-owned buffer from the driver.  Allocate a heap block
     * for the caller, copy data, then return the slab slot right away.
     * This keeps the driver slab free so DMA never stalls waiting for slots.
     */
    size_t expected = pcm_block_size(&src->cfg.stream);
    if (drv_bytes < expected) {
        LOG_WRN("i2s_source: short read %zu < %zu bytes", drv_bytes, expected);
        /* Still proceed – partial blocks are unusual but not fatal */
    }

    /* Allocate heap storage for the PCM data */
    void *heap_data = k_malloc(drv_bytes);
    if (!heap_data) {
        LOG_ERR("i2s_source: heap alloc failed (%zu bytes)", drv_bytes);
        k_mem_slab_free(src->cfg.slab, drv_buf);
        src->base.stats.dropped++;
        return -ENOMEM;
    }

    memcpy(heap_data, drv_buf, drv_bytes);

    /* Return slab slot to driver immediately */
    k_mem_slab_free(src->cfg.slab, drv_buf);

    /* Populate the caller's block descriptor */
    out_block->info = src->cfg.stream;
    out_block->data = heap_data;
    out_block->size = drv_bytes;
    out_block->seq  = src->seq++;

    src->base.stats.blocks_ok++;
    return 0;
}

/**
 * release() – free a block obtained from read().
 *
 * Ownership: caller returns the block; after this call data pointer
 *            is invalid.
 */
static void i2s_source_release_impl(pcm_source_t *base, pcm_block_t *block)
{
    ARG_UNUSED(base);
    if (block && block->data) {
        k_free(block->data);
        block->data = NULL;
        block->size = 0;
    }
}

/** start() – configure the device then trigger DMA start. */
static int i2s_source_start_impl(pcm_source_t *base)
{
    i2s_source_t *src = to_i2s_src(base);

    if (src->running) {
        LOG_WRN("i2s_source: already running");
        return 0;
    }

    struct i2s_config cfg = {
        .word_size      = pcm_bytes_per_sample(src->cfg.stream.format) * 8u,
        .channels       = src->cfg.stream.channels,
        .format         = I2S_FMT_DATA_FORMAT_I2S,
        .options        = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
        .frame_clk_freq = src->cfg.stream.sample_rate,
        .mem_slab       = src->cfg.slab,
        .block_size     = pcm_block_size(&src->cfg.stream),
        .timeout        = (int32_t)src->cfg.driver_timeout_ms,
    };

    int err = i2s_configure(src->cfg.dev, I2S_DIR_RX, &cfg);
    if (err < 0) {
        LOG_ERR("i2s_source: configure failed: %d", err);
        return err;
    }

    err = i2s_trigger(src->cfg.dev, I2S_DIR_RX, I2S_TRIGGER_START);
    if (err < 0) {
        LOG_ERR("i2s_source: trigger START failed: %d", err);
        return err;
    }

    src->running = true;
    LOG_INF("i2s_source: started (%u Hz, %u ch, %u frames/block)",
            src->cfg.stream.sample_rate,
            src->cfg.stream.channels,
            src->cfg.stream.frames);
    return 0;
}

/** stop() – trigger PREPARE to quiesce DMA. */
static int i2s_source_stop_impl(pcm_source_t *base)
{
    i2s_source_t *src = to_i2s_src(base);

    if (!src->running) return 0;

    int err = i2s_trigger(src->cfg.dev, I2S_DIR_RX, I2S_TRIGGER_STOP);
    if (err < 0) {
        LOG_WRN("i2s_source: STOP trigger failed: %d", err);
    }

    /* PREPARE clears any error state so the device can be restarted later */
    i2s_trigger(src->cfg.dev, I2S_DIR_RX, I2S_TRIGGER_PREPARE);

    src->running = false;
    LOG_INF("i2s_source: stopped  (blocks_ok=%u overruns=%u restarts=%u)",
            src->base.stats.blocks_ok,
            src->base.stats.overruns,
            src->base.stats.restarts);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public init                                                         */
/* ------------------------------------------------------------------ */

int i2s_source_init(i2s_source_t *src, const i2s_source_cfg_t *cfg)
{
    if (!src || !cfg || !cfg->dev || !cfg->slab) {
        return -EINVAL;
    }

    if (!device_is_ready(cfg->dev)) {
        LOG_ERR("i2s_source: device not ready");
        return -ENODEV;
    }

    memset(src, 0, sizeof(*src));

    /* Populate vtable */
    src->base.read    = i2s_source_read_impl;
    src->base.release = i2s_source_release_impl;
    src->base.start   = i2s_source_start_impl;
    src->base.stop    = i2s_source_stop_impl;
    src->base.info    = cfg->stream;

    /* Store config */
    src->cfg     = *cfg;
    src->running = false;
    src->seq     = 0;

    LOG_INF("i2s_source: initialised on %s", cfg->dev->name);
    return 0;
}
