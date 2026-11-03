#include "gem_ai.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/dsp/image/processing.hpp"

static const char *TAG = "ai";

#define IN_W EI_CLASSIFIER_INPUT_WIDTH
#define IN_H EI_CLASSIFIER_INPUT_HEIGHT

static uint8_t *s_input;  // IN_W x IN_H RGB888 once resized
static size_t s_input_size;

// Edge Impulse wants each pixel as one float holding 0xRRGGBB
static int get_pixels(size_t offset, size_t length, float *out)
{
    const uint8_t *p = s_input + offset * 3;
    for (size_t i = 0; i < length; i++, p += 3) {
        out[i] = (float)((p[0] << 16) | (p[1] << 8) | p[2]);
    }
    return 0;
}

extern "C" int gem_ai_label_count(void) { return EI_CLASSIFIER_LABEL_COUNT; }
extern "C" const char *gem_ai_label(int i) { return ei_classifier_inferencing_categories[i]; }
extern "C" const char *gem_ai_name(void)
{
    return CONFIG_GEMCAM_MODEL_NAME[0] ? CONFIG_GEMCAM_MODEL_NAME : EI_CLASSIFIER_PROJECT_NAME;
}
extern "C" int gem_ai_version(void) { return EI_CLASSIFIER_PROJECT_DEPLOY_VERSION; }

extern "C" esp_err_t gem_ai_classify(const uint8_t *rgb, int width, int height, float *scores, int *ms)
{
    // The resize crops into the output buffer at full size before shrinking it
    // in place, so the buffer must hold the cropped image, not just IN_W x IN_H
    const size_t need = (size_t)width * height * 3;
    if (need > s_input_size) {
        heap_caps_free(s_input);
        s_input = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
        s_input_size = s_input ? need : 0;
        if (!s_input) {
            return ESP_ERR_NO_MEM;
        }
    }
    // Same crop/resize mode the impulse was trained with (center square for "fit shortest")
    if (ei::image::processing::resize_image_using_mode(rgb, width, height, s_input, IN_W, IN_H, 3,
                                                       EI_CLASSIFIER_RESIZE_MODE) != 0) {
        return ESP_FAIL;
    }

    signal_t signal;
    signal.total_length = IN_W * IN_H;
    signal.get_data = &get_pixels;

    ei_impulse_result_t result;
    memset(&result, 0, sizeof(result));
    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
    if (err != EI_IMPULSE_OK) {
        ESP_LOGE(TAG, "run_classifier failed (%d)", (int)err);
        return ESP_FAIL;
    }
    for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        scores[i] = result.classification[i].value;
    }
    *ms = result.timing.dsp + result.timing.classification;
    return ESP_OK;
}
