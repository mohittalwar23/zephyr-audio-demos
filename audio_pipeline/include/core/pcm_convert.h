/*
 * pcm_convert.h / pcm_convert.c  –  Reusable format/channel conversion stage
 *
 * Mentor feedback: "keep channel/format conversion as separate reusable
 *                   processing stages rather than burying them in a backend."
 *
 * In the original loopback the INMP441 mono→stereo expansion lived inside
 * play_node().  This stage lifts it out so any pipeline can use it.
 *
 * Currently supported conversions:
 *   • Mono → Stereo  (S16_LE):  duplicate L channel to R
 *   • Stereo → Mono  (S16_LE):  average L+R
 *   • Gain (int16 with clamping) — previously #define GAIN in play_node()
 *
 * Ownership (same contract as every pipeline stage):
 *   pcm_convert_*() receives a block (owns data), allocates a new output
 *   block (caller owns it), frees the input block before returning.
 */

#ifndef PCM_CONVERT_H
#define PCM_CONVERT_H

#include "core/pcm_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Expand a mono S16_LE block to interleaved stereo S16_LE.
 *
 * The INMP441 outputs on the LEFT channel only (right = 0).
 * This stage reads src[i*2] (L) and writes L to both channels of the output.
 *
 * Ownership: in_block->data is freed; out_block->data is heap-allocated.
 *
 * @param in_block   Input block (mono, channels=1 or stereo with dead R).
 * @param out_block  Output block (stereo, channels=2).
 * @param gain       Integer gain factor (1 = unity, 2..8 = louder).
 *                   Applied with int32 intermediate and int16 clamping.
 * @return 0 on success, -ENOMEM if heap alloc fails.
 */
int pcm_mono_to_stereo_s16(const pcm_block_t *in_block,
                            pcm_block_t       *out_block,
                            int                gain);

/**
 * @brief Mix interleaved stereo S16_LE down to mono S16_LE.
 *
 * Output sample = (L + R) / 2, with clamping.
 *
 * Ownership: in_block->data is freed; out_block->data is heap-allocated.
 *
 * @param in_block   Input block  (stereo, channels=2).
 * @param out_block  Output block (mono,   channels=1).
 * @return 0 on success, -ENOMEM if heap alloc fails.
 */
int pcm_stereo_to_mono_s16(const pcm_block_t *in_block,
                            pcm_block_t       *out_block);

/**
 * @brief Apply gain in-place to an S16_LE block.
 *
 * Modifies block->data directly; no reallocation.
 *
 * @param block  Block to modify.
 * @param gain   Integer gain factor (clamped to int16 range).
 */
void pcm_apply_gain_s16(pcm_block_t *block, int gain);

#ifdef __cplusplus
}
#endif

#endif /* PCM_CONVERT_H */
