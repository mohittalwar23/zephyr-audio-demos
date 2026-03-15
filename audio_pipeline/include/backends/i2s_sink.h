/*
 * i2s_sink.h  –  I2S backend for pcm_sink_t
 *
 * Wraps a Zephyr I2S TX device into a pcm_sink_t.
 *
 * Buffer ownership:
 *   - write() allocates a slot from the TX slab, copies the caller's
 *     block data into it, then hands that slab slot to i2s_write().
 *   - The I2S driver owns the slab slot until the DMA completes; the
 *     driver then frees it automatically (standard Zephyr I2S contract).
 *   - The incoming pcm_block_t (heap memory) is freed by write() after
 *     the copy, honouring the sink ownership transfer guarantee.
 *
 * Underrun / overrun handling:
 *   - TX slab exhaustion → stats.overruns++, write() returns -ENOBUFS.
 *   - i2s_write() failure → block dropped, stats.dropped++,
 *     auto-restart attempted, stats.restarts++.
 *   - Caller can detect restarts and re-prefill silence if desired.
 *
 * Silence prefill:
 *   - i2s_sink_prefill_silence() queues N zero-filled blocks before
 *     I2S_TRIGGER_START to avoid underrun at the very beginning.
 */

#ifndef I2S_SINK_H
#define I2S_SINK_H

#include "core/pcm_pipeline.h"
#include <zephyr/drivers/i2s.h>

#ifdef __cplusplus
extern "C" {
#endif

/** I2S-specific configuration for the TX backend. */
typedef struct {
    const struct device *dev;              /**< Zephyr I2S device (TX direction) */
    struct k_mem_slab   *slab;             /**< Slab used by the I2S driver       */
    pcm_stream_info_t    stream;           /**< Expected stream format             */
    uint32_t             driver_timeout_ms;/**< slab alloc timeout (ms)           */
    uint8_t              prefill_blocks;   /**< Silence blocks queued before START */
} i2s_sink_cfg_t;

/** Concrete I2S sink – first member IS pcm_sink_t for safe casting. */
typedef struct {
    pcm_sink_t       base;    /**< MUST be first – enables safe cast */
    i2s_sink_cfg_t   cfg;
    bool             running;
} i2s_sink_t;

/**
 * @brief Initialise an I2S TX sink backend.
 *
 * Configures the I2S device and populates the vtable.
 * Does NOT start DMA – call pcm_sink_start() for that.
 *
 * @param sink  Uninitialized i2s_sink_t to fill in.
 * @param cfg   Configuration (device, slab, stream parameters).
 * @return 0 on success, negative errno otherwise.
 */
int i2s_sink_init(i2s_sink_t *sink, const i2s_sink_cfg_t *cfg);

/**
 * @brief Pre-queue silence blocks to prevent TX underrun at startup.
 *
 * Call BEFORE pcm_sink_start().
 *
 * @param sink          Sink handle.
 * @param n_blocks      Number of zero-filled blocks to queue.
 * @return 0 on success, negative errno on slab alloc or write failure.
 */
int i2s_sink_prefill_silence(i2s_sink_t *sink, uint8_t n_blocks);

#ifdef __cplusplus
}
#endif

#endif /* I2S_SINK_H */
