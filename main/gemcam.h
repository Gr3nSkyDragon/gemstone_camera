#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_camera.h"

// ---- camera.c ---------------------------------------------------------------

// Starts the camera, hard-resetting and retrying if it doesn't come up cleanly
esp_err_t gem_camera_init(void);
// Full power-cycle and restart; takes the camera lock
esp_err_t gem_camera_restart(void);
bool gem_camera_ok(void);
const char *gem_camera_error(void);

// Only one task may use the camera at a time (web server + BOOT button)
void gem_camera_lock(void);
void gem_camera_unlock(void);
// Like esp_camera_fb_get(), but restarts the camera if frames stop arriving.
// Call with the camera lock held.
camera_fb_t *gem_camera_grab(void);

// Sensor controls by name ("awb", "brightness", ...). Saved to flash.
esp_err_t gem_camera_set(const char *name, int value);
// Writes {"name":value,...} for every control into buf
int gem_camera_settings_json(char *buf, size_t len);

// ---- gem.c ------------------------------------------------------------------

#define GEM_LABEL_LEN 24
#define GEM_FEAT_N 16
#define GEM_MAX_LABELS 24

typedef enum {
    GEM_MODE_TRAINED,  // the Edge Impulse neural network (components/ei_model)
    GEM_MODE_LIVE,     // color matching against samples taught on the web page
    GEM_MODE_COUNT
} gem_mode_t;

typedef struct {
    float f[GEM_FEAT_N];
} gem_features_t;

typedef struct {
    char label[GEM_LABEL_LEN];
    float score;  // 0..1; all scores add up to 1
} gem_score_t;

typedef struct {
    unsigned seq;         // increases with every identification
    gem_mode_t mode;      // which method produced it
    char label[GEM_LABEL_LEN];
    float confidence;     // score of the winning label
    char runner_up[GEM_LABEL_LEN];  // "" if only one label exists
    int ms;               // time spent capturing + identifying
    int nscores;
    gem_score_t scores[GEM_MAX_LABELS];  // every label, highest first
} gem_result_t;

esp_err_t gem_store_init(void);

// Captures `frames` frames and averages their color features. Takes the camera lock.
esp_err_t gem_capture_features(int frames, gem_features_t *out);

// Cleans a user-typed label in place; returns false if nothing usable is left
bool gem_sanitize_label(char *label);

// Which method Identify uses (saved to flash)
gem_mode_t gem_mode(void);
esp_err_t gem_set_mode(gem_mode_t mode);
const char *gem_mode_name(gem_mode_t mode);
bool gem_mode_from_name(const char *name, gem_mode_t *out);

// Live demo samples
esp_err_t gem_teach(const char *label, const gem_features_t *f);
// label == NULL forgets everything
esp_err_t gem_forget(const char *label);
// Returns false when nothing has been taught
bool gem_identify_live(const gem_features_t *f, gem_result_t *out);

// Capture + identify with the current mode + remember + show;
// ESP_ERR_NOT_FOUND when the live demo has nothing taught yet
esp_err_t gem_identify_now(gem_result_t *out);
// Runs the trained model on an uploaded JPEG through exactly the same decode,
// crop and resize as a camera frame. Doesn't change the last result.
// If `input` is given, also copies the RGB888 image the model saw into it
// (needs input_w * input_h * 3 bytes; 96x96 is 27648).
esp_err_t gem_classify_jpeg(const uint8_t *jpg, size_t len, gem_result_t *out,
                            uint8_t *input, size_t input_size, int *input_w, int *input_h);
// Runs the AI model once and logs the result and timing (first runs are slow)
void gem_ai_warmup(void);
// The most recent result from any source (web page or BOOT button)
bool gem_last_result(gem_result_t *out);
unsigned gem_result_seq(void);

int gem_live_count(void);
// Writes [{"label":"x","count":n},...] into buf
int gem_live_labels_json(char *buf, size_t len);
// Writes {"name":"...","version":n,"labels":[{"label":"x"},...]} into buf
int gem_ai_info_json(char *buf, size_t len);
int gem_result_json(const gem_result_t *r, char *buf, size_t len);

// Prints the result to the serial console (and later, the SSD1306)
void gem_show_result(const gem_result_t *r);

// ---- web.c ------------------------------------------------------------------

esp_err_t gem_web_start(void);
