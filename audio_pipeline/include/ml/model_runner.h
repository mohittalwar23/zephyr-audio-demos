/*
 * Copyright 2025 The TensorFlow Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MICRO_SPEECH_XIAO_MODEL_RUNNER_H_
#define MICRO_SPEECH_XIAO_MODEL_RUNNER_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocate and initialize both TFLM interpreters.
 *
 * Must be called once before micro_speech_process_audio().
 */
void model_runner_init(void);

/**
 * @brief Run the full audio-preprocessor + keyword-spotting pipeline.
 *
 * Generates MFSC features from @p audio_data and runs the micro-speech
 * classifier.  The detected label is printed via the Zephyr logging
 * subsystem.
 *
 * @param audio_data      16-bit PCM samples at 16 kHz mono.
 * @param audio_data_size Number of samples.
 *
 * @return 0 on success, -1 if feature generation failed,
 *         -2 if inference failed.
 */
int micro_speech_process_audio(const int16_t *audio_data,
			       size_t audio_data_size);

#ifdef __cplusplus
}
#endif

#endif /* MICRO_SPEECH_XIAO_MODEL_RUNNER_H_ */
