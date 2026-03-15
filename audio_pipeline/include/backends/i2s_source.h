/*
 * i2s_source.h  –  I2S backend for pcm_source_t
 *
 * Wraps a Zephyr I2S RX device into a pcm_source_t.
 *
 * Buffer ownership:
 *   - The underlying Zephyr I2S driver returns slab-owned blocks.
 *   - i2s_source_read() copies the DMA buffer into a heap-allocated
 *     pcm_block_t so the slab slot is freed immediately, keeping the
 *     driver slab lean and avoiding stalls.
 *   - Caller receives heap ownership; must call pcm_source_release()
 *     which calls k_free().
 *
 * Error / overrun handling:
 *   - If the RX slab is momentarily exhausted (driver drops a block),
 *     stats.overruns is incremented and read() returns -ENOBUFS.
 *   - On I2S driver error the device is automatically re-triggered and
 *     stats.restarts is incremented.
 */

#ifndef I2S_SOURCE_H
#define I2S_SOURCE_H

#include "core/pcm_pipeline.h"
#include <zephyr/drivers/i2s.h>

#ifdef __cplusplus
extern "C" {
#endif

/** I2S-specific configuration for the RX backend. */
typedef struct {
    const struct device *dev;          /**< Zephyr I2S device (RX direction) */
    struct k_mem_slab   *slab;         /**< Slab used by the I2S driver       */
    pcm_stream_info_t    stream;       /**< Desired stream format              */
    uint32_t             driver_timeout_ms; /**< i2s_read() timeout            */
} i2s_source_cfg_t;

/** Concrete I2S source – first member IS pcm_source_t for safe casting. */
typedef struct {
    pcm_source_t      base;           /**< MUST be first – enables safe cast  */
    i2s_source_cfg_t  cfg;
    bool              running;
    uint32_t          seq;            /**< Block sequence counter             */
} i2s_source_t;

/**
 * @brief Initialise an I2S RX source backend.
 *
 * Configures the I2S device and populates the vtable.
 * Does NOT start DMA – call pcm_source_start() for that.
 *
 * @param src  Uninitialized i2s_source_t to fill in.
 * @param cfg  Configuration (device, slab, stream parameters).
 * @return 0 on success, negative errno otherwise.
 */
int i2s_source_init(i2s_source_t *src, const i2s_source_cfg_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* I2S_SOURCE_H */
