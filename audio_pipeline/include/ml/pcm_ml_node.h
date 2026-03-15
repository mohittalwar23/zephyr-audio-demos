/*
 * pcm_ml_node.h  –  Keyword-spotting tap as a pipeline node
 *
 * Memory layout (ESP32 friendly):
 *   - Inference thread stack: static K_THREAD_STACK_DEFINE (not heap)
 *   - msgq stores only a flag (not a 32KB audio copy) — audio lives in
 *     staging_buf which the inference thread reads directly under a mutex
 *   - staging_buf + write_half stay in BSS (48KB total, unavoidable for
 *     1-second 16kHz window)
 */

#ifndef PCM_ML_NODE_H
#define PCM_ML_NODE_H

#include "core/pcm_pipeline.h"
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Sizes                                                               */
/* ------------------------------------------------------------------ */
#define ML_SAMPLE_RATE        16000
#define ML_INFERENCE_SAMPLES  16000   /* 1 second at 16 kHz */
#define ML_TRIGGER_SAMPLES     8000   /* 500 ms trigger stride */
#define ML_INFER_STACK_SIZE    4096   /* static stack size */

/* ------------------------------------------------------------------ */
/* Result callback                                                     */
/* ------------------------------------------------------------------ */
typedef void (*ml_result_cb_t)(const char *label, float score, void *ctx);

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t        input_channels;  /* 1=mono, 2=stereo(extracts left) */
    ml_result_cb_t result_cb;
    void          *result_cb_ctx;
    int            infer_priority;
} pcm_ml_node_cfg_t;

/* ------------------------------------------------------------------ */
/* Node state                                                          */
/* ------------------------------------------------------------------ */

/**
 * Declare the inference thread stack statically — avoids k_thread_stack_alloc
 * which pulls from libc heap (limited to ~22KB on ESP32).
 * Place this macro once in your main_ml.c alongside the node declaration:
 *
 *   PCM_ML_NODE_STACK_DEFINE(ml_stack);
 *   static pcm_ml_node_t ml_node;
 *
 * Then pass ml_stack to pcm_ml_node_init().
 */
#define PCM_ML_NODE_STACK_DEFINE(name) \
    K_THREAD_STACK_DEFINE(name, ML_INFER_STACK_SIZE)

typedef struct {
    pcm_ml_node_cfg_t cfg;

    /* Sliding window buffers — 48KB BSS total, unavoidable */
    int16_t  staging_buf[ML_INFERENCE_SAMPLES];
    int16_t  write_half[ML_TRIGGER_SAMPLES];
    uint32_t write_pos;

    /*
     * Signalling: pipeline thread sets ready=true and gives sem;
     * inference thread takes sem, reads staging_buf under mutex, runs TFLM.
     * No 32KB copy in the queue — staging_buf is shared under mutex.
     */
    struct k_sem    trigger_sem;   /* given when 500ms window is ready */
    struct k_mutex  buf_mutex;     /* protects staging_buf during copy  */

    /* Inference thread — stack passed in from static definition */
    struct k_thread  infer_thread_data;
    k_thread_stack_t *infer_stack; /* points to PCM_ML_NODE_STACK_DEFINE */

    /* Stats */
    uint32_t triggers_sent;
    uint32_t triggers_dropped;
    uint32_t infer_ok;
    uint32_t infer_err;

    bool initialized;
} pcm_ml_node_t;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/**
 * @param node        Uninitialised pcm_ml_node_t.
 * @param cfg         Configuration.
 * @param infer_stack Stack from PCM_ML_NODE_STACK_DEFINE().
 */
int pcm_ml_node_init(pcm_ml_node_t     *node,
                     const pcm_ml_node_cfg_t *cfg,
                     k_thread_stack_t  *infer_stack);

void pcm_ml_node_process(pcm_ml_node_t *node, const pcm_block_t *block);
void pcm_ml_node_log_stats(const pcm_ml_node_t *node);

#ifdef __cplusplus
}
#endif

#endif /* PCM_ML_NODE_H */
