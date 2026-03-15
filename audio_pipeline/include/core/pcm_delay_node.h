/*
 * pcm_delay_node.h  –  Rolling delay as a reusable pipeline node
 *
 * Mentor feedback: "treat the rolling delay as a pipeline node,
 *                   not as a special-case property of the loopback app."
 *
 * A pcm_delay_node sits between a source and a sink:
 *
 *     pcm_source_t  →  pcm_delay_node_t  →  pcm_sink_t
 *
 * It accumulates DELAY_BLOCKS worth of PCM blocks in an internal FIFO
 * before releasing any to the output.  After that, every pushed block
 * causes one block to be popped and returned — maintaining a fixed delay.
 *
 * Observability:
 *   - stats.queue_depth: live depth of the internal FIFO
 *   - stats.dropped: blocks evicted when heap is full
 *   - stats.overruns: pushes while backpressure is active
 *
 * Thread safety: NOT thread-safe.  Use from a single pipeline thread.
 */

#ifndef PCM_DELAY_NODE_H
#define PCM_DELAY_NODE_H

#include "core/pcm_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Singly-linked node holding one PCM block in the delay FIFO. */
typedef struct pcm_delay_entry {
    struct pcm_delay_entry *next;
    pcm_block_t             block;  /**< Owns block.data (heap) */
} pcm_delay_entry_t;

/** Configuration for the delay node. */
typedef struct {
    uint32_t delay_blocks;   /**< Target delay in blocks                   */
    uint32_t max_blocks;     /**< Hard cap; 0 = same as delay_blocks + 10  */
} pcm_delay_node_cfg_t;

/** Delay node state. */
typedef struct {
    pcm_delay_node_cfg_t cfg;

    /* Internal FIFO */
    pcm_delay_entry_t *head;    /**< Oldest block (next to play)   */
    pcm_delay_entry_t *tail;    /**< Newest block (last recorded)  */
    uint32_t           depth;   /**< Current queue depth           */

    bool               primed;  /**< True once delay buffer is full */

    /* Observable statistics */
    pcm_stats_t stats;
} pcm_delay_node_t;

/**
 * @brief Initialise the delay node.
 *
 * @param node  Node to initialise.
 * @param cfg   Configuration.
 * @return 0 on success, -EINVAL on bad parameters.
 */
int pcm_delay_node_init(pcm_delay_node_t *node, const pcm_delay_node_cfg_t *cfg);

/**
 * @brief Push a block into the delay FIFO.
 *
 * Ownership: node takes ownership of block->data on success.
 *            On failure caller still owns it.
 *
 * @param node   Delay node.
 * @param block  Block to enqueue (node takes ownership of block->data).
 * @return 0 on success, -ENOMEM if heap is exhausted.
 */
int pcm_delay_node_push(pcm_delay_node_t *node, pcm_block_t *block);

/**
 * @brief Pop a block from the front of the FIFO (once primed).
 *
 * Returns -EAGAIN if the delay buffer is not yet full (still filling).
 * Returns -ENODATA if the queue is unexpectedly empty.
 *
 * Ownership: caller receives block->data and MUST call k_free(block->data).
 *
 * @param node       Delay node.
 * @param out_block  Populated on success.
 * @return 0 on success, -EAGAIN if not primed, -ENODATA if queue empty.
 */
int pcm_delay_node_pop(pcm_delay_node_t *node, pcm_block_t *out_block);

/**
 * @brief True once the delay FIFO has accumulated delay_blocks blocks.
 */
static inline bool pcm_delay_node_is_primed(const pcm_delay_node_t *node)
{
    return node->primed;
}

/**
 * @brief Flush and free all queued blocks.
 *
 * Resets the node to un-primed state. Safe to call at shutdown.
 */
void pcm_delay_node_flush(pcm_delay_node_t *node);

/**
 * @brief Return a pointer to the node's live statistics.
 */
static inline const pcm_stats_t *pcm_delay_node_stats(const pcm_delay_node_t *node)
{
    return &node->stats;
}

#ifdef __cplusplus
}
#endif

#endif /* PCM_DELAY_NODE_H */
