/*
 * pcm_ml_node.c  –  Keyword-spotting tap pipeline node
 *
 * Key ESP32 memory fixes vs previous version:
 *   1. Stack is static (K_THREAD_STACK_DEFINE), not heap-allocated
 *   2. No 32KB msgq copy — inference reads staging_buf directly under mutex
 *   3. sem-based signalling (k_sem) instead of k_msgq
 */

#include "ml/pcm_ml_node.h"
#include "ml/model_runner.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(pcm_ml_node, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/* Inference thread                                                    */
/* ------------------------------------------------------------------ */

static void infer_thread_fn(void *arg1, void *arg2, void *arg3)
{
    pcm_ml_node_t *node = (pcm_ml_node_t *)arg1;
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    /* Local 1-second buffer — lives on this thread's 4KB stack.
     * 16000 samples × 2 bytes = 32KB — TOO BIG for stack.
     * Must be static here. */
    static int16_t infer_buf[ML_INFERENCE_SAMPLES];

    LOG_INF("pcm_ml_node: inference thread started");

    while (1) {
        /* Wait for a trigger from the pipeline thread */
        k_sem_take(&node->trigger_sem, K_FOREVER);

        /* Copy staging_buf under mutex so pipeline can keep writing */
        k_mutex_lock(&node->buf_mutex, K_FOREVER);
        memcpy(infer_buf, node->staging_buf,
               ML_INFERENCE_SAMPLES * sizeof(int16_t));
        k_mutex_unlock(&node->buf_mutex);

        int ret = micro_speech_process_audio(infer_buf, ML_INFERENCE_SAMPLES);
        if (ret == 0) {
            node->infer_ok++;
            if (node->cfg.result_cb) {
                node->cfg.result_cb("(see log)", 0.0f,
                                    node->cfg.result_cb_ctx);
            }
        } else {
            node->infer_err++;
            LOG_ERR("pcm_ml_node: inference failed (%d)", ret);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Mono extraction                                                     */
/* ------------------------------------------------------------------ */

static void extract_mono(const pcm_block_t *block,
                         int16_t *dst, uint32_t frames)
{
    const int16_t *src = (const int16_t *)block->data;
    uint8_t ch = block->info.channels;

    if (ch == 1) {
        memcpy(dst, src, frames * sizeof(int16_t));
    } else {
        for (uint32_t i = 0; i < frames; i++) {
            dst[i] = src[i * ch];  /* left channel */
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int pcm_ml_node_init(pcm_ml_node_t          *node,
                     const pcm_ml_node_cfg_t *cfg,
                     k_thread_stack_t        *infer_stack)
{
    if (!node || !cfg || !infer_stack)       return -EINVAL;
    if (cfg->input_channels == 0 ||
        cfg->input_channels > 2)             return -EINVAL;

    memset(node, 0, sizeof(*node));
    node->cfg         = *cfg;
    node->infer_stack = infer_stack;

    k_sem_init(&node->trigger_sem, 0, 1);
    k_mutex_init(&node->buf_mutex);

    model_runner_init();

    k_tid_t tid = k_thread_create(
        &node->infer_thread_data,
        infer_stack,
        ML_INFER_STACK_SIZE,
        infer_thread_fn,
        node, NULL, NULL,
        cfg->infer_priority, 0, K_NO_WAIT);

    k_thread_name_set(tid, "ml_infer");

    node->initialized = true;
    LOG_INF("pcm_ml_node: init OK  ch=%u  prio=%d",
            cfg->input_channels, cfg->infer_priority);
    return 0;
}

void pcm_ml_node_process(pcm_ml_node_t *node, const pcm_block_t *block)
{
    if (!node->initialized || !block || !block->data) return;

    if (block->info.sample_rate != ML_SAMPLE_RATE ||
        block->info.format      != PCM_FMT_S16_LE) {
        LOG_WRN_ONCE("pcm_ml_node: format mismatch");
        return;
    }

    uint32_t frames = block->info.frames;
    int16_t mono[frames];
    extract_mono(block, mono, frames);

    uint32_t src_pos = 0;
    while (src_pos < frames) {
        uint32_t space   = ML_TRIGGER_SAMPLES - node->write_pos;
        uint32_t to_copy = MIN(frames - src_pos, space);

        memcpy(node->write_half + node->write_pos,
               mono + src_pos,
               to_copy * sizeof(int16_t));

        node->write_pos += to_copy;
        src_pos         += to_copy;

        if (node->write_pos >= ML_TRIGGER_SAMPLES) {
            /* Slide staging_buf and append new 500ms chunk */
            k_mutex_lock(&node->buf_mutex, K_FOREVER);
            memmove(node->staging_buf,
                    node->staging_buf + ML_TRIGGER_SAMPLES,
                    ML_TRIGGER_SAMPLES * sizeof(int16_t));
            memcpy(node->staging_buf + ML_TRIGGER_SAMPLES,
                   node->write_half,
                   ML_TRIGGER_SAMPLES * sizeof(int16_t));
            k_mutex_unlock(&node->buf_mutex);

            node->write_pos = 0;

            /* Signal inference thread — non-blocking, drop if busy.
             * k_sem_give() is void in Zephyr 4.x; use count to detect
             * whether previous trigger was already consumed. */
            if (k_sem_count_get(&node->trigger_sem) == 0) {
                k_sem_give(&node->trigger_sem);
                node->triggers_sent++;
            } else {
                node->triggers_dropped++;
            }
        }
    }
}

void pcm_ml_node_log_stats(const pcm_ml_node_t *node)
{
    LOG_INF("ml_node: sent=%u dropped=%u ok=%u err=%u",
            node->triggers_sent, node->triggers_dropped,
            node->infer_ok, node->infer_err);
}
