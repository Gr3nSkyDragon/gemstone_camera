#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_camera.h"

// ---- camera.c ---------------------------------------------------------------

esp_err_t gem_camera_init(void);
bool gem_camera_ok(void);
const char *gem_camera_error(void);

// Only one task may use the camera at a time (web server + BOOT button)
void gem_camera_lock(void);
void gem_camera_unlock(void);

// Sensor controls by name ("awb", "brightness", ...). Saved to flash.
esp_err_t gem_camera_set(const char *name, int value);
// Writes {"name":value,...} for every control into buf
int gem_camera_settings_json(char *buf, size_t len);

// ---- gem.c ------------------------------------------------------------------

#define GEM_LABEL_LEN 24
#define GEM_FEAT_N 16
#define GEM_MAX_SAMPLES 300

typedef struct {
    float f[GEM_FEAT_N];
} gem_features_t;

typedef struct {
    char label[GEM_LABEL_LEN];
    float confidence;     // 0..1, share of the k-nearest-neighbour vote
    float distance;       // distance to the closest sample of that label
    char runner_up[GEM_LABEL_LEN];  // "" if only one label voted
    int ms;               // time spent capturing + matching
} gem_result_t;

esp_err_t gem_store_init(void);

// Captures `frames` frames and averages their features. Takes the camera lock.
esp_err_t gem_capture_features(int frames, gem_features_t *out);

// Cleans a user-typed label in place; returns false if nothing usable is left
bool gem_sanitize_label(char *label);

esp_err_t gem_teach(const char *label, const gem_features_t *f);
// Returns false when nothing has been taught yet
bool gem_identify(const gem_features_t *f, gem_result_t *out);
// label == NULL forgets everything
esp_err_t gem_forget(const char *label);

int gem_sample_count(void);
// Writes [{"label":"x","count":n},...] into buf
int gem_labels_json(char *buf, size_t len);

// Capture + match + show; ESP_ERR_NOT_FOUND when nothing has been taught
esp_err_t gem_identify_now(gem_result_t *out);

// Prints the result to the serial console (and later, the SSD1306)
void gem_show_result(const gem_result_t *r);

// ---- web.c ------------------------------------------------------------------

esp_err_t gem_web_start(void);
