#pragma once

// C interface to the Edge Impulse image classifier in model/

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

int gem_ai_label_count(void);
const char *gem_ai_label(int i);
// Display name (menuconfig, else the Edge Impulse project name) and deploy version
const char *gem_ai_name(void);
int gem_ai_version(void);

// Classifies an RGB888 image of any size. It is cropped and resized the same
// way Edge Impulse Studio prepared the training photos. Writes one score per
// label (adding up to 1) and the time inference took.
esp_err_t gem_ai_classify(const uint8_t *rgb, int width, int height, float *scores, int *ms);

// The RGB888 image the last gem_ai_classify() call fed the model, after the
// crop and resize (NULL before the first call). Only valid until the next call.
const uint8_t *gem_ai_last_input(int *width, int *height);

#ifdef __cplusplus
}
#endif
