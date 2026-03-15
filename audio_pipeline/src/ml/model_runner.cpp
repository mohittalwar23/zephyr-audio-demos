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

#include "ml/model_runner.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(model_runner);

#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"

/*
 * Model selection via CMake variable (set in CMakeLists.txt):
 *   -DMICRO_SPEECH_FLOAT_PREPROCESSOR=y  → float32 audio preprocessor
 *   (default)                            → INT8  audio preprocessor
 * The keyword classifier is always INT8 quantized.
 */
#if defined(MICRO_SPEECH_FLOAT_PREPROCESSOR)
#include "ml/audio_preprocessor_float_model.hpp"
#define g_audio_preprocessor_model_data g_audio_preprocessor_float_model
#else
#include "ml/audio_preprocessor_int8_model.hpp"
#define g_audio_preprocessor_model_data g_audio_preprocessor_int8_model
#endif

#include "ml/micro_speech_quantized_model.hpp"
#include "ml/micro_model_settings.h"

#include <zephyr/kernel.h>
#include <algorithm>
#include <string.h>
#include <math.h>

/* -----------------------------------------------------------------------
 * Type aliases
 * ----------------------------------------------------------------------- */
using MicroSpeechOpResolver    = tflite::MicroMutableOpResolver<4>;
using AudioPreprocessorOpResolver = tflite::MicroMutableOpResolver<18>;

using Features = int8_t[kFeatureCount][kFeatureSize];

/* -----------------------------------------------------------------------
 * Tensor arenas
 * ----------------------------------------------------------------------- */
namespace {

#if defined(MICRO_SPEECH_FLOAT_PREPROCESSOR)
constexpr size_t kAudioPreprocessorArenaSize = 32768; /* ~32 KB for float32 ops */
#else
constexpr size_t kAudioPreprocessorArenaSize = 16384; /* ~16 KB for INT8 ops */
#endif
constexpr size_t kMicroSpeechArenaSize       = 12200; /* ~12 KB */

alignas(16) uint8_t g_audio_preprocessor_arena[kAudioPreprocessorArenaSize];
alignas(16) uint8_t g_micro_speech_arena[kMicroSpeechArenaSize];

/* Derived sample counts */
constexpr int kAudioSampleDurationCount =
	kFeatureDurationMs * kAudioSampleFrequency / 1000;
constexpr int kAudioSampleStrideCount =
	kFeatureStrideMs * kAudioSampleFrequency / 1000;

/* Static interpreters – allocated once */
static const tflite::Model *g_audio_preprocessor_model;
static const tflite::Model *g_micro_speech_model;
static AudioPreprocessorOpResolver *g_audio_preprocessor_op_resolver;
static MicroSpeechOpResolver       *g_micro_speech_op_resolver;
static tflite::MicroInterpreter    *g_audio_preprocessor_interpreter;
static tflite::MicroInterpreter    *g_micro_speech_interpreter;
static bool g_initialized;

/* Feature buffer */
static Features g_features;

/* -----------------------------------------------------------------------
 * Op registration helpers
 * ----------------------------------------------------------------------- */
static TfLiteStatus register_micro_speech_ops(MicroSpeechOpResolver &r)
{
	TF_LITE_ENSURE_STATUS(r.AddReshape());
	TF_LITE_ENSURE_STATUS(r.AddFullyConnected());
	TF_LITE_ENSURE_STATUS(r.AddDepthwiseConv2D());
	TF_LITE_ENSURE_STATUS(r.AddSoftmax());
	return kTfLiteOk;
}

static TfLiteStatus register_audio_preprocessor_ops(AudioPreprocessorOpResolver &r)
{
	TF_LITE_ENSURE_STATUS(r.AddReshape());
	TF_LITE_ENSURE_STATUS(r.AddCast());
	TF_LITE_ENSURE_STATUS(r.AddStridedSlice());
	TF_LITE_ENSURE_STATUS(r.AddConcatenation());
	TF_LITE_ENSURE_STATUS(r.AddMul());
	TF_LITE_ENSURE_STATUS(r.AddAdd());
	TF_LITE_ENSURE_STATUS(r.AddDiv());
	TF_LITE_ENSURE_STATUS(r.AddMinimum());
	TF_LITE_ENSURE_STATUS(r.AddMaximum());
	TF_LITE_ENSURE_STATUS(r.AddWindow());
	TF_LITE_ENSURE_STATUS(r.AddFftAutoScale());
	TF_LITE_ENSURE_STATUS(r.AddRfft());
	TF_LITE_ENSURE_STATUS(r.AddEnergy());
	TF_LITE_ENSURE_STATUS(r.AddFilterBank());
	TF_LITE_ENSURE_STATUS(r.AddFilterBankSquareRoot());
	TF_LITE_ENSURE_STATUS(r.AddFilterBankSpectralSubtraction());
	TF_LITE_ENSURE_STATUS(r.AddPCAN());
	TF_LITE_ENSURE_STATUS(r.AddFilterBankLog());
	return kTfLiteOk;
}

/* -----------------------------------------------------------------------
 * Interpreter initialisation
 * ----------------------------------------------------------------------- */
static TfLiteStatus initialize_interpreters(void)
{
	if (g_initialized) {
		return kTfLiteOk;
	}

#if defined(MICRO_SPEECH_FLOAT_PREPROCESSOR)
	LOG_INF("Initializing TFLM interpreters [preprocessor: float32]");
#else
	LOG_INF("Initializing TFLM interpreters [preprocessor: INT8 quantized]");
#endif

	/* Audio preprocessor */
	g_audio_preprocessor_model =
		tflite::GetModel(g_audio_preprocessor_model_data);
	if (g_audio_preprocessor_model->version() != TFLITE_SCHEMA_VERSION) {
		LOG_ERR("Audio preprocessor model schema mismatch");
		return kTfLiteError;
	}

	static AudioPreprocessorOpResolver audio_preprocessor_op_resolver;
	g_audio_preprocessor_op_resolver = &audio_preprocessor_op_resolver;
	if (register_audio_preprocessor_ops(*g_audio_preprocessor_op_resolver)
	    != kTfLiteOk) {
		LOG_ERR("Failed to register audio preprocessor ops");
		return kTfLiteError;
	}

	static tflite::MicroInterpreter audio_preprocessor_interpreter(
		g_audio_preprocessor_model,
		*g_audio_preprocessor_op_resolver,
		g_audio_preprocessor_arena,
		kAudioPreprocessorArenaSize);
	g_audio_preprocessor_interpreter = &audio_preprocessor_interpreter;
	if (g_audio_preprocessor_interpreter->AllocateTensors() != kTfLiteOk) {
		LOG_ERR("Audio preprocessor tensor allocation failed");
		return kTfLiteError;
	}

	/* Micro speech classifier */
	g_micro_speech_model =
		tflite::GetModel(g_micro_speech_quantized_model);
	if (g_micro_speech_model->version() != TFLITE_SCHEMA_VERSION) {
		LOG_ERR("Micro speech model schema mismatch");
		return kTfLiteError;
	}

	static MicroSpeechOpResolver micro_speech_op_resolver;
	g_micro_speech_op_resolver = &micro_speech_op_resolver;
	if (register_micro_speech_ops(*g_micro_speech_op_resolver) != kTfLiteOk) {
		LOG_ERR("Failed to register micro speech ops");
		return kTfLiteError;
	}

	static tflite::MicroInterpreter micro_speech_interpreter(
		g_micro_speech_model,
		*g_micro_speech_op_resolver,
		g_micro_speech_arena,
		kMicroSpeechArenaSize);
	g_micro_speech_interpreter = &micro_speech_interpreter;
	if (g_micro_speech_interpreter->AllocateTensors() != kTfLiteOk) {
		LOG_ERR("Micro speech tensor allocation failed");
		return kTfLiteError;
	}

	g_initialized = true;
	LOG_INF("TFLM interpreters initialized");
	return kTfLiteOk;
}

/* -----------------------------------------------------------------------
 * Feature generation
 * ----------------------------------------------------------------------- */
static TfLiteStatus generate_single_feature(const int16_t *audio_data,
					    int audio_data_size,
					    int8_t *feature_output)
{
	TfLiteTensor *input =
		g_audio_preprocessor_interpreter->input(0);
	if (!input) {
		return kTfLiteError;
	}
	if (audio_data_size != kAudioSampleDurationCount) {
		LOG_ERR("Feature window size mismatch: %d vs %d",
			audio_data_size, kAudioSampleDurationCount);
		return kTfLiteError;
	}

	TfLiteTensor *output =
		g_audio_preprocessor_interpreter->output(0);
	if (!output) {
		return kTfLiteError;
	}

	std::copy_n(audio_data, audio_data_size,
		    tflite::GetTensorData<int16_t>(input));

	if (g_audio_preprocessor_interpreter->Invoke() != kTfLiteOk) {
		LOG_ERR("Audio preprocessor invoke failed");
		return kTfLiteError;
	}

#if defined(MICRO_SPEECH_FLOAT_PREPROCESSOR)
	/*
	 * Float preprocessor emits float32 features.  Quantize them to int8
	 * using the classifier input tensor's own scale/zero_point so the
	 * values land in the range the INT8 classifier was trained on.
	 */
	{
		const float *f = tflite::GetTensorData<float>(output);
		TfLiteTensor *cls_in = g_micro_speech_interpreter->input(0);
		const float scale      = cls_in->params.scale;
		const int   zero_point = cls_in->params.zero_point;

		for (int i = 0; i < kFeatureSize; i++) {
			int32_t q = static_cast<int32_t>(roundf(f[i] / scale))
				    + zero_point;
			q = std::max(-128, std::min(127, q));
			feature_output[i] = static_cast<int8_t>(q);
		}
	}
#else
	std::copy_n(tflite::GetTensorData<int8_t>(output),
		    kFeatureSize, feature_output);
#endif
	return kTfLiteOk;
}

static TfLiteStatus generate_features(const int16_t *audio_data,
				      size_t audio_data_size,
				      Features *features_output)
{
	memset(features_output, 0, sizeof(Features));

	size_t remaining    = audio_data_size;
	size_t feature_idx  = 0;
	const int16_t *ptr  = audio_data;

	while (remaining >= (size_t)kAudioSampleDurationCount &&
	       feature_idx < kFeatureCount) {
		if (generate_single_feature(ptr, kAudioSampleDurationCount,
					    (*features_output)[feature_idx])
		    != kTfLiteOk) {
			LOG_ERR("Feature %zu generation failed", feature_idx);
			return kTfLiteError;
		}
		feature_idx++;
		ptr       += kAudioSampleStrideCount;
		remaining -= kAudioSampleStrideCount;
	}

	return kTfLiteOk;
}

/* -----------------------------------------------------------------------
 * Inference
 * ----------------------------------------------------------------------- */
static TfLiteStatus run_inference(const Features &features)
{
	TfLiteTensor *input = g_micro_speech_interpreter->input(0);
	TfLiteTensor *output = g_micro_speech_interpreter->output(0);

	if (!input || !output) {
		return kTfLiteError;
	}
	if (output->dims->data[output->dims->size - 1] != kCategoryCount) {
		LOG_ERR("Output tensor size mismatch");
		return kTfLiteError;
	}

	std::copy_n(&features[0][0], kFeatureElementCount,
		    tflite::GetTensorData<int8_t>(input));

	if (g_micro_speech_interpreter->Invoke() != kTfLiteOk) {
		LOG_ERR("Micro speech invoke failed");
		return kTfLiteError;
	}

	float output_scale      = output->params.scale;
	int   output_zero_point = output->params.zero_point;

	float scores[kCategoryCount];
	const int8_t *raw = tflite::GetTensorData<int8_t>(output);

	for (int i = 0; i < kCategoryCount; i++) {
		scores[i] = (raw[i] - output_zero_point) * output_scale;
	}

	int best = (int)(std::max_element(scores, scores + kCategoryCount) -
			 scores);
	LOG_INF("Detected: %s", kCategoryLabels[best]);

	return kTfLiteOk;
}

} /* namespace */

/* -----------------------------------------------------------------------
 * Public C API
 * ----------------------------------------------------------------------- */
extern "C" {

void model_runner_init(void)
{
	if (initialize_interpreters() != kTfLiteOk) {
		LOG_ERR("Failed to initialize TFLM interpreters");
	}
}

int micro_speech_process_audio(const int16_t *audio_data,
			       size_t audio_data_size)
{
	if (generate_features(audio_data, audio_data_size,
			      &g_features) != kTfLiteOk) {
		LOG_ERR("Feature generation failed");
		return -1;
	}
	if (run_inference(g_features) != kTfLiteOk) {
		LOG_ERR("Inference failed");
		return -2;
	}
	return 0;
}

} /* extern "C" */
