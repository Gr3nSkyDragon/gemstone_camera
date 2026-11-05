// Works around a bug in ESP-NN's ESP32-S3 assembly convolution, as shipped in
// the Edge Impulse SDK: layers with fewer than 8 input channels (the model's
// first layer, which reads the RGB image) come out wrong. On the same 96x96
// inputs the board scored 59/100 against 99/100 for the SDK's reference
// kernels on a PC, with jasper read as lapis lazuli and citrine as quartz.
//
// The linker redirects every call to esp_nn_conv_s8_esp32s3() here (see
// --wrap in CMakeLists.txt). Few-channel layers use ESP-NN's plain C version,
// which agrees with the reference; all other layers keep the fast assembly.
// This lives outside model/ so it survives replacing the model with a new export.

#include "esp_nn_ansi_headers.h"

#define MIN_ASM_CHANNELS 8

void __real_esp_nn_conv_s8_esp32s3(const data_dims_t *input_dims, const int8_t *input,
                                   const data_dims_t *filter_dims, const int8_t *filter_data,
                                   const int32_t *bias, const data_dims_t *output_dims,
                                   int8_t *out_data, const conv_params_t *conv_params,
                                   const quant_data_t *quant_data);

void __wrap_esp_nn_conv_s8_esp32s3(const data_dims_t *input_dims, const int8_t *input,
                                   const data_dims_t *filter_dims, const int8_t *filter_data,
                                   const int32_t *bias, const data_dims_t *output_dims,
                                   int8_t *out_data, const conv_params_t *conv_params,
                                   const quant_data_t *quant_data)
{
    if (input_dims->channels < MIN_ASM_CHANNELS) {
        esp_nn_conv_s8_ansi(input_dims, input, filter_dims, filter_data, bias,
                            output_dims, out_data, conv_params, quant_data);
        return;
    }
    __real_esp_nn_conv_s8_esp32s3(input_dims, input, filter_dims, filter_data, bias,
                                  output_dims, out_data, conv_params, quant_data);
}
