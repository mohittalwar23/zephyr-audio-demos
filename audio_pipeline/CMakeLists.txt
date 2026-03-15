cmake_minimum_required(VERSION 3.20.0)

# ---------------------------------------------------------------------------
# Pipeline variant — must be set BEFORE find_package so EXTRA_CONF_FILE works
#
#   west build . -- -DPIPELINE=loopback
#   west build .                          (ml is default)
# ---------------------------------------------------------------------------
if(NOT DEFINED PIPELINE)
    set(PIPELINE "ml")
endif()
message(STATUS "audio_pipeline: PIPELINE=${PIPELINE}")

if(PIPELINE STREQUAL "loopback")
    set(EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/prj_loopback.conf)
else()
    set(EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/prj_ml.conf)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(audio_pipeline
    VERSION 0.1.0
    LANGUAGES C CXX
)

# ---------------------------------------------------------------------------
# ESP32 C++ ABI fix (ML only)
# ---------------------------------------------------------------------------
if(PIPELINE STREQUAL "ml")
    set(NO_THREADSAFE_STATICS $<TARGET_PROPERTY:compiler-cpp,no_threadsafe_statics>)
    zephyr_compile_options($<$<COMPILE_LANGUAGE:CXX>:${NO_THREADSAFE_STATICS}>)
endif()

# ---------------------------------------------------------------------------
# Include paths — single root, subdirs accessed as core/ backends/ ml/
# ---------------------------------------------------------------------------
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ---------------------------------------------------------------------------
# Core sources — always compiled
# ---------------------------------------------------------------------------
target_sources(app PRIVATE
    src/core/pcm_delay_node.c
    src/core/pcm_convert.c
)

# ---------------------------------------------------------------------------
# Backend sources — always compiled (both pipelines use I2S)
# ---------------------------------------------------------------------------
target_sources(app PRIVATE
    src/backends/i2s_source.c
    src/backends/i2s_sink.c
)

# ---------------------------------------------------------------------------
# Variant-specific sources
# ---------------------------------------------------------------------------
if(PIPELINE STREQUAL "loopback")
    target_sources(app PRIVATE src/main.c)

else()
    # ML pipeline
    if(MICRO_SPEECH_FLOAT_PREPROCESSOR)
        message(STATUS "audio_pipeline: preprocessor = float32")
        zephyr_compile_definitions(MICRO_SPEECH_FLOAT_PREPROCESSOR=1)
        set(PREPROCESSOR_SRC src/ml/audio_preprocessor_float_model.cpp)
    else()
        message(STATUS "audio_pipeline: preprocessor = INT8 quantized (default)")
        set(PREPROCESSOR_SRC src/ml/audio_preprocessor_int8_model.cpp)
    endif()

    set(TFLM_SIGNAL_DIR ${ZEPHYR_TFLITE_MICRO_MODULE_DIR}/signal)

    target_include_directories(app PRIVATE
        ${ZEPHYR_TFLITE_MICRO_MODULE_DIR}/signal
        ${ZEPHYR_TFLITE_MICRO_MODULE_DIR}/signal/micro/kernels
        ${ZEPHYR_TFLITE_MICRO_MODULE_DIR}/signal/src
    )

    target_sources(app PRIVATE
        src/main_ml.c
        src/ml/pcm_ml_node.c
        src/ml/model_runner.cpp
        src/ml/micro_speech_quantized_model.cpp
        ${PREPROCESSOR_SRC}
        # TFLite signal processing sources
        ${TFLM_SIGNAL_DIR}/src/fft_auto_scale.cc
        ${TFLM_SIGNAL_DIR}/src/max_abs.cc
        ${TFLM_SIGNAL_DIR}/src/msb_32.cc
        ${TFLM_SIGNAL_DIR}/src/energy.cc
        ${TFLM_SIGNAL_DIR}/src/filter_bank.cc
        ${TFLM_SIGNAL_DIR}/src/filter_bank_square_root.cc
        ${TFLM_SIGNAL_DIR}/src/square_root_64.cc
        ${TFLM_SIGNAL_DIR}/src/square_root_32.cc
        ${TFLM_SIGNAL_DIR}/src/msb_64.cc
        ${TFLM_SIGNAL_DIR}/src/pcan_argc_fixed.cc
        ${TFLM_SIGNAL_DIR}/src/filter_bank_log.cc
        ${TFLM_SIGNAL_DIR}/src/log.cc
        ${TFLM_SIGNAL_DIR}/src/filter_bank_spectral_subtraction.cc
        ${TFLM_SIGNAL_DIR}/micro/kernels/energy.cc
        ${TFLM_SIGNAL_DIR}/micro/kernels/fft_auto_scale_common.cc
        ${TFLM_SIGNAL_DIR}/micro/kernels/fft_auto_scale_kernel.cc
        ${TFLM_SIGNAL_DIR}/micro/kernels/filter_bank_square_root_common.cc
        ${TFLM_SIGNAL_DIR}/micro/kernels/filter_bank_square_root.cc
        ${TFLM_SIGNAL_DIR}/micro/kernels/filter_bank_spectral_subtraction.cc
        ${TFLM_SIGNAL_DIR}/micro/kernels/filter_bank.cc
        ${TFLM_SIGNAL_DIR}/micro/kernels/pcan.cc
        ${TFLM_SIGNAL_DIR}/micro/kernels/filter_bank_log.cc
    )
endif()