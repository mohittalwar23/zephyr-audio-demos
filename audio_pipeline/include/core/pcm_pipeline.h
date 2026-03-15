/*
 * pcm_pipeline.h  –  Common PCM source / sink / node interface
 *
 * Design goals (per mentor feedback):
 *   - Backend-agnostic: processing stages see only PCM blocks + metadata
 *   - Explicit buffer ownership: every alloc/free path is documented
 *   - Observability built-in: stats struct on every endpoint
 *   - Backpressure: overrun/underrun reported, not silently dropped
 *
 * Ownership rules (MUST be followed by every backend):
 *   SOURCE side:
 *     pcm_source_read()  → caller receives ownership of *block
 *     pcm_source_release() → caller returns ownership (frees the slab slot)
 *
 *   SINK side:
 *     pcm_sink_write()   → callee takes ownership of block; caller must NOT
 *                          free it.  Sink frees from its own slab after DMA.
 *
 *   Processing nodes (e.g. delay, gain):
 *     Receive a block (own it), produce a new block (own it),
 *     free the input block before returning.
 */

#ifndef PCM_PIPELINE_H
#define PCM_PIPELINE_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* PCM frame metadata – the only thing the processing stage ever sees  */
/* ------------------------------------------------------------------ */

/** Supported sample formats (extend as needed). */
typedef enum {
    PCM_FMT_S16_LE = 0,   /**< Signed 16-bit little-endian (most common)  */
    PCM_FMT_S24_LE,       /**< Signed 24-bit little-endian                 */
    PCM_FMT_S32_LE,       /**< Signed 32-bit little-endian                 */
} pcm_format_t;

/** Immutable description of a PCM stream. */
typedef struct {
    uint32_t     sample_rate;   /**< e.g. 16000, 44100, 48000             */
    uint8_t      channels;      /**< 1 = mono, 2 = stereo, …              */
    pcm_format_t format;        /**< Sample width / endianness             */
    uint16_t     frames;        /**< Frames per block (NOT bytes)          */
} pcm_stream_info_t;

/** One block of PCM audio handed between pipeline stages. */
typedef struct {
    pcm_stream_info_t info;     /**< Metadata – valid for this block only  */
    void             *data;     /**< Raw interleaved PCM samples           */
    size_t            size;     /**< Byte length of data                   */
    uint32_t          seq;      /**< Monotonic block sequence number       */
} pcm_block_t;

/* ------------------------------------------------------------------ */
/* Observability – attach one of these to every source / sink          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t blocks_ok;         /**< Successfully transferred blocks       */
    uint32_t overruns;          /**< RX: slab full; TX: driver queue full  */
    uint32_t underruns;         /**< TX: no data available in time         */
    uint32_t dropped;           /**< Blocks discarded due to backpressure  */
    uint32_t restarts;          /**< Driver error → restart count          */
    uint32_t queue_depth;       /**< Current pipeline depth (live)         */
} pcm_stats_t;

/* ------------------------------------------------------------------ */
/* Source interface                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Abstract PCM source handle.
 *
 * A concrete backend (I2S, DMIC, USB, …) embeds this as its first member
 * so it can be cast freely between pcm_source_t * and the concrete type.
 */
typedef struct pcm_source pcm_source_t;

struct pcm_source {
    /**
     * @brief Read one PCM block from the source.
     *
     * Blocks until a block is available or timeout expires.
     *
     * Ownership: on success (*block) is owned by the caller.
     *            Caller MUST call pcm_source_release() when done.
     *
     * @param src     Source handle.
     * @param block   OUT – populated with block pointer and metadata.
     * @param timeout Kernel timeout (K_FOREVER, K_NO_WAIT, K_MSEC(n)).
     * @return 0 on success, negative errno on error.
     */
    int (*read)(pcm_source_t *src, pcm_block_t *block, k_timeout_t timeout);

    /**
     * @brief Release a block back to the source's memory pool.
     *
     * MUST be called for every successful read() unless the block is
     * handed off to a sink (which takes ownership).
     *
     * @param src   Source handle.
     * @param block Block previously obtained from read().
     */
    void (*release)(pcm_source_t *src, pcm_block_t *block);

    /** Start the source (trigger DMA / interrupt). */
    int (*start)(pcm_source_t *src);

    /** Stop the source cleanly. */
    int (*stop)(pcm_source_t *src);

    /** Stream description produced by this source. */
    pcm_stream_info_t info;

    /** Live statistics – updated by the backend. */
    pcm_stats_t stats;
};

/* Convenience wrappers */
static inline int  pcm_source_read(pcm_source_t *s, pcm_block_t *b, k_timeout_t t)
                   { return s->read(s, b, t); }
static inline void pcm_source_release(pcm_source_t *s, pcm_block_t *b)
                   { s->release(s, b); }
static inline int  pcm_source_start(pcm_source_t *s) { return s->start(s); }
static inline int  pcm_source_stop(pcm_source_t *s)  { return s->stop(s); }

/* ------------------------------------------------------------------ */
/* Sink interface                                                       */
/* ------------------------------------------------------------------ */

typedef struct pcm_sink pcm_sink_t;

struct pcm_sink {
    /**
     * @brief Write one PCM block to the sink.
     *
     * Ownership: on success the SINK owns *block and will free it after
     *            the DMA transfer completes.  Caller must NOT access block
     *            after this call returns 0.
     * On failure: ownership stays with the caller; caller must release.
     *
     * @param sink    Sink handle.
     * @param block   Block to play/transmit.
     * @param timeout Kernel timeout.
     * @return 0 on success, negative errno on error.
     */
    int (*write)(pcm_sink_t *sink, pcm_block_t *block, k_timeout_t timeout);

    /** Start the sink. */
    int (*start)(pcm_sink_t *sink);

    /** Drain (flush pending blocks then stop). */
    int (*drain)(pcm_sink_t *sink);

    /** Hard stop – discard queued blocks. */
    int (*stop)(pcm_sink_t *sink);

    /** Stream description accepted by this sink. */
    pcm_stream_info_t info;

    /** Live statistics – updated by the backend. */
    pcm_stats_t stats;
};

static inline int pcm_sink_write(pcm_sink_t *s, pcm_block_t *b, k_timeout_t t)
                  { return s->write(s, b, t); }
static inline int pcm_sink_start(pcm_sink_t *s)  { return s->start(s); }
static inline int pcm_sink_drain(pcm_sink_t *s)  { return s->drain(s); }
static inline int pcm_sink_stop(pcm_sink_t *s)   { return s->stop(s); }

/* ------------------------------------------------------------------ */
/* Utility                                                             */
/* ------------------------------------------------------------------ */

/** Bytes per sample for a given format. */
static inline uint8_t pcm_bytes_per_sample(pcm_format_t fmt)
{
    switch (fmt) {
    case PCM_FMT_S16_LE: return 2;
    case PCM_FMT_S24_LE: return 3;
    case PCM_FMT_S32_LE: return 4;
    default:             return 2;
    }
}

/** Bytes for one complete block given its metadata. */
static inline size_t pcm_block_size(const pcm_stream_info_t *info)
{
    return (size_t)info->frames * info->channels * pcm_bytes_per_sample(info->format);
}

#ifdef __cplusplus
}
#endif

#endif /* PCM_PIPELINE_H */
