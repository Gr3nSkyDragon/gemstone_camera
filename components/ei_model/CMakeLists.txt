# The Edge Impulse model, exported from Studio as a "C++ library" and unzipped
# into model/. To update the model, replace model/ with a new export.
set(EI "${CMAKE_CURRENT_LIST_DIR}/model")
set(SDK "${EI}/edge-impulse-sdk")

# Only what an image model needs on an ESP32-S3: the compiled model, the
# classifier, image DSP, TensorFlow Lite Micro and the Espressif port.
# CMSIS is for Arm chips and is skipped. ESP-NN (Espressif's neural-network
# kernels) is built here rather than as its own component because its sources
# include Edge Impulse headers.
set(ESPNN "${SDK}/porting/espressif/ESP-NN")
file(GLOB_RECURSE ESPNN_SRCS "${ESPNN}/src/*_ansi.c" "${ESPNN}/src/*_opt.c"
                             "${ESPNN}/src/*_esp32s3.c" "${ESPNN}/src/*_esp32s3.S")
file(GLOB_RECURSE EI_SRCS
    "${EI}/tflite-model/*.cpp"
    "${SDK}/classifier/*.cpp"
    "${SDK}/dsp/*.cpp"
    "${SDK}/tensorflow/lite/*.cc"
    "${SDK}/tensorflow/lite/*.c")
file(GLOB EI_PORT_SRCS "${SDK}/porting/espressif/*.cpp")

idf_component_register(SRCS "gem_ai.cpp" ${EI_SRCS} ${EI_PORT_SRCS} ${ESPNN_SRCS}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "model"
                                         "model/edge-impulse-sdk"
                                         "model/edge-impulse-sdk/third_party/flatbuffers/include"
                                         "model/edge-impulse-sdk/third_party/gemmlowp"
                                         "model/edge-impulse-sdk/third_party/ruy"
                                         "model/edge-impulse-sdk/porting/espressif/ESP-NN/include"
                                         "model/edge-impulse-sdk/porting/espressif/ESP-NN/src/common"
                       PRIV_REQUIRES esp_timer)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    EIDSP_USE_ESP_DSP=0          # only audio models need esp-dsp's FFT
    EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=1
    TF_LITE_STATIC_MEMORY)
if(CONFIG_IDF_TARGET_ESP32S3)
    # ESP-NN's S3 assembly checks this, and .S files never see sdkconfig.h,
    # where the SDK would otherwise derive it from CONFIG_IDF_TARGET_ESP32S3
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN_S3=1)
endif()
# Vendored code: don't let its warnings fail the build. -Os because GCC 13's
# Xtensa backend crashes (internal compiler error) on parts of the SDK at -O2;
# ESP-NN, which does the heavy maths, is still built at -O2.
target_compile_options(${COMPONENT_LIB} PRIVATE -w -Os)
set_source_files_properties(${ESPNN_SRCS} PROPERTIES COMPILE_OPTIONS "-O2")
