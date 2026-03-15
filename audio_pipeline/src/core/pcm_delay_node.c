/*
 * pcm_delay_node.c  –  Rolling delay pipeline node implementation
 */

#include "core/pcm_delay_node.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(pcm_delay, LOG_LEVEL_DBG);

/* ------------------------------------------------------------------ */
/* Internal FIFO helpers                                               */
/* ------------------------------------------------------------------ */

static void fifo_push(pcm_delay_node_t *node, pcm_delay_entry_t *entry)
{
    entry->next = NULL;
    if (node->tail) node->tail->next = entry;
    else            node->head = entry;
    node->tail = entry;
    node->depth++;
    node->stats.queue_depth = node->depth;
}

static pcm_delay_entry_t *fifo_pop(pcm_delay_node_t *node)
{
    if (!node->head) return NULL;
    pcm_delay_entry_t *e = node->head;
    node->head = e->next;
    if (!node->head) node->tail = NULL;
    node->depth--;
    node->stats.queue_depth = node->depth;
    return e;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int pcm_delay_node_init(pcm_delay_node_t *node, const pcm_delay_node_cfg_t *cfg)
{
    if (!node || !cfg || cfg->delay_blocks == 0) {
        return -EINVAL;
    }

    memset(node, 0, sizeof(*node));
    node->cfg = *cfg;

    /* If max_blocks not set, add a small headroom above the target delay */
    if (node->cfg.max_blocks == 0) {
        node->cfg.max_blocks = cfg->delay_blocks + 10u;
    }

    LOG_INF("pcm_delay: init  delay=%u blocks  max=%u blocks",
            node->cfg.delay_blocks, node->cfg.max_blocks);
    return 0;
}

int pcm_delay_node_push(pcm_delay_node_t *node, pcm_block_t *block)
{
    /*
     * If the queue is at the hard cap, evict the oldest block to make room.
     * This mirrors the original loopback's heap-full eviction strategy while
     * keeping the logic here instead of in application code.
     */
    if (node->depth >= node->cfg.max_blocks) {
        LOG_WRN("pcm_delay: FIFO at cap (%u) — evicting oldest block", node->depth);
        pcm_delay_entry_t *old = fifo_pop(node);
        if (old) {
            k_free(old->block.data);
            k_free(old);
            node->stats.dropped++;
        }
    }

    /* Allocate a FIFO entry to wrap the block */
    pcm_delay_entry_t *entry = k_malloc(sizeof(pcm_delay_entry_t));
    if (!entry) {
        LOG_ERR("pcm_delay: no heap for FIFO entry");
        node->stats.overruns++;
        return -ENOMEM;
    }

    /*
     * Transfer ownership of block->data into the entry.
     * The caller must NOT free block->data after this returns 0.
     */
    entry->block = *block;
    entry->next  = NULL;
    block->data  = NULL;    /* Prevent accidental double-free by caller */

    fifo_push(node, entry);
    node->stats.blocks_ok++;

    /* Check if we've just reached the target delay depth */
    if (!node->primed && node->depth >= node->cfg.delay_blocks) {
        node->primed = true;
        LOG_INF("pcm_delay: PRIMED at %u blocks", node->depth);
    }

    return 0;
}

int pcm_delay_node_pop(pcm_delay_node_t *node, pcm_block_t *out_block)
{
    if (!node->primed) {
        /* Still accumulating — caller should not play yet */
        return -EAGAIN;
    }

    pcm_delay_entry_t *entry = fifo_pop(node);
    if (!entry) {
        /*
         * Should not happen in normal operation because push primes first,
         * but guard against it gracefully.
         */
        LOG_WRN("pcm_delay: pop on empty queue (underrun)");
        node->stats.underruns++;
        node->primed = false;   /* Re-prime on next push cycle */
        return -ENODATA;
    }

    /*
     * Transfer ownership of block->data to the caller.
     * Caller MUST call k_free(out_block->data) when done,
     * or pass it to a sink (which takes ownership via pcm_sink_write).
     */
    *out_block = entry->block;
    k_free(entry);   /* Free the FIFO wrapper; data is now caller's */

    return 0;
}

void pcm_delay_node_flush(pcm_delay_node_t *node)
{
    pcm_delay_entry_t *e;
    uint32_t freed = 0;

    while ((e = fifo_pop(node)) != NULL) {
        k_free(e->block.data);
        k_free(e);
        freed++;
    }

    node->primed = false;
    LOG_INF("pcm_delay: flushed %u blocks", freed);
}
