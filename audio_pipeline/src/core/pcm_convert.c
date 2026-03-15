/*
 * pcm_convert.c  –  Reusable format/channel conversion pipeline stage
 */

#include "core/pcm_convert.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(pcm_convert, LOG_LEVEL_DBG);

/* ------------------------------------------------------------------ */
/* Mono → Stereo                                                       */
/* ------------------------------------------------------------------ */

int pcm_mono_to_stereo_s16(const pcm_block_t *in_block,
                            pcm_block_t       *out_block,
                            int                gain)
{
    if (!in_block || !out_block || !in_block->data) {
        return -EINVAL;
    }

    uint32_t frames = in_block->info.frames;

    /* Output: same frame count, 2 channels, S16_LE */
    size_t out_bytes = (size_t)frames * 2u * sizeof(int16_t);
    int16_t *dst = k_malloc(out_bytes);
    if (!dst) {
        LOG_ERR("pcm_convert: mono→stereo alloc failed (%zu bytes)", out_bytes);
        return -ENOMEM;
    }

    /*
     * INMP441 delivers left-channel data at even indices in a 2-ch stream.
     * If the input was configured as 1-channel, just use index 0, 1, 2...
     */
    int16_t *src      = (int16_t *)in_block->data;
    uint8_t  in_step  = in_block->info.channels;  /* 1 or 2 */

    for (uint32_t i = 0; i < frames; i++) {
        int32_t s = (int32_t)src[i * in_step] * gain;

        /* Clamp to int16 — prevents wrap-around distortion at high gain */
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;

        dst[i * 2]     = (int16_t)s;   /* Left  */
        dst[i * 2 + 1] = (int16_t)s;   /* Right */
    }

    /* Populate output block */
    out_block->info             = in_block->info;
    out_block->info.channels    = 2;
    out_block->info.format      = PCM_FMT_S16_LE;
    out_block->data             = dst;
    out_block->size             = out_bytes;
    out_block->seq              = in_block->seq;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Stereo → Mono                                                       */
/* ------------------------------------------------------------------ */

int pcm_stereo_to_mono_s16(const pcm_block_t *in_block,
                            pcm_block_t       *out_block)
{
    if (!in_block || !out_block || !in_block->data) {
        return -EINVAL;
    }
    if (in_block->info.channels != 2) {
        LOG_ERR("pcm_convert: stereo→mono requires 2 input channels, got %u",
                in_block->info.channels);
        return -EINVAL;
    }

    uint32_t frames    = in_block->info.frames;
    size_t   out_bytes = (size_t)frames * 1u * sizeof(int16_t);

    int16_t *dst = k_malloc(out_bytes);
    if (!dst) {
        LOG_ERR("pcm_convert: stereo→mono alloc failed (%zu bytes)", out_bytes);
        return -ENOMEM;
    }

    int16_t *src = (int16_t *)in_block->data;
    for (uint32_t i = 0; i < frames; i++) {
        int32_t mixed = ((int32_t)src[i * 2] + (int32_t)src[i * 2 + 1]) / 2;
        if (mixed >  32767) mixed =  32767;
        if (mixed < -32768) mixed = -32768;
        dst[i] = (int16_t)mixed;
    }

    out_block->info          = in_block->info;
    out_block->info.channels = 1;
    out_block->data          = dst;
    out_block->size          = out_bytes;
    out_block->seq           = in_block->seq;

    return 0;
}

/* ------------------------------------------------------------------ */
/* In-place gain                                                       */
/* ------------------------------------------------------------------ */

void pcm_apply_gain_s16(pcm_block_t *block, int gain)
{
    if (!block || !block->data || gain == 1) return;

    int16_t *samples = (int16_t *)block->data;
    size_t   n       = block->size / sizeof(int16_t);

    for (size_t i = 0; i < n; i++) {
        int32_t s = (int32_t)samples[i] * gain;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        samples[i] = (int16_t)s;
    }
}
