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
    GEM_BANK_TRAINED,  // from a photo dataset (tools/train.py); not touched by the demo
    GEM_BANK_LIVE,     // taught on the web page; wipe for a live demo
    GEM_BANK_COUNT
} gem_bank_t;

typedef struct {
    float f[GEM_FEAT_N];
} gem_features_t;

typedef struct {
    char label[GEM_LABEL_LEN];
    float score;  // 0..1; all scores add up to 1
} gem_score_t;

typedef struct {
    unsigned seq;         // increases with every identification
    gem_bank_t bank;      // which samples were used
    char label[GEM_LABEL_LEN];
    float confidence;     // score of the winning label
    float distance;       // its mean distance to its closest samples
    char runner_up[GEM_LABEL_LEN];  // "" if only one label exists
    int ms;               // time spent capturing + matching
    int nscores;
    gem_score_t scores[GEM_MAX_LABELS];  // every label, highest first
} gem_result_t;

esp_err_t gem_store_init(void);

// Captures `frames` frames and averages their features. Takes the camera lock.
esp_err_t gem_capture_features(int frames, gem_features_t *out);

// Cleans a user-typed label in place; returns false if nothing usable is left
bool gem_sanitize_label(char *label);

// Which bank Identify uses (saved to flash)
gem_bank_t gem_mode(void);
esp_err_t gem_set_mode(gem_bank_t bank);
const char *gem_bank_name(gem_bank_t bank);
bool gem_bank_from_name(const char *name, gem_bank_t *out);

// Live bank
esp_err_t gem_teach(const char *label, const gem_features_t *f);
// label == NULL forgets everything
esp_err_t gem_forget(const char *label);

// Trained bank: clear, add photos, then commit to save them
esp_err_t gem_train_clear(void);
esp_err_t gem_train_add_jpeg(const char *label, const uint8_t *jpg, size_t len);
esp_err_t gem_train_commit(void);

// Returns false when the bank is empty
bool gem_identify(gem_bank_t bank, const gem_features_t *f, gem_result_t *out);
// Capture + match with the current mode + remember + show;
// ESP_ERR_NOT_FOUND when that bank is empty
esp_err_t gem_identify_now(gem_result_t *out);
// The most recent result from any source (web page or BOOT button)
bool gem_last_result(gem_result_t *out);
unsigned gem_result_seq(void);

int gem_sample_count(gem_bank_t bank);
// Writes [{"label":"x","count":n},...] into buf
int gem_labels_json(gem_bank_t bank, char *buf, size_t len);
int gem_result_json(const gem_result_t *r, char *buf, size_t len);

// Prints the result to the serial console (and later, the SSD1306)
void gem_show_result(const gem_result_t *r);

// ---- web.c ------------------------------------------------------------------

esp_err_t gem_web_start(void);
