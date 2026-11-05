// Identifies stones two ways, picked with the mode switch on the web page:
//
//   trained  the Edge Impulse neural network in components/ei_model, trained
//            online from the photo dataset. Fixed until the model is re-exported.
//   live     classic computer vision: color/texture features of the middle of
//            the picture, matched against samples taught on the web page a few
//            presses at a time. Wipe and re-teach at will for a live demo.

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "gem_ai.h"
#include "gemcam.h"

static const char *TAG = "gem";

#define STORE_PART "gemdata"
#define STORE_NS   "gems"
#define STORE_VER  "fver"
#define STORE_MODE "mode"
// "samples" is the key the earlier versions used, so earlier teaching is kept
#define STORE_LIVE "samples"
// Bump when the feature layout changes; old samples are then discarded
#define FEATURE_VERSION 1

#define LIVE_MAX 300
// Each label's distance is the mean of its K closest samples
#define K_NEIGHBORS 3
#define HUE_BINS 8

typedef struct {
    char label[GEM_LABEL_LEN];
    float f[GEM_FEAT_N];
} sample_t;

static const char *MODE_NAMES[GEM_MODE_COUNT] = {
    [GEM_MODE_TRAINED] = "trained",
    [GEM_MODE_LIVE] = "live",
};

// Feature layout (all roughly 0..1):
//   0-2   mean R, G, B
//   3-5   standard deviation of R, G, B  (texture, banding, sparkle)
//   6-13  hue histogram of colored pixels, 8 bins of 45 degrees
//   14    fraction of gray/white/black pixels
//   15    mean saturation
static const float WEIGHTS[GEM_FEAT_N] = {
    2, 2, 2,
    1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1,
    1,
};

static SemaphoreHandle_t s_lock;   // guards the live samples, mode and last result
static sample_t *s_live;
static int s_live_count;
static gem_mode_t s_mode = GEM_MODE_TRAINED;
static gem_result_t s_last;
static unsigned s_seq;

static uint8_t *s_rgb;             // decode buffer; only used under the camera lock
static size_t s_rgb_size;
static int s_rgb_w, s_rgb_h;

// ---- storage ----------------------------------------------------------------

static esp_err_t save_live(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(STORE_PART, STORE_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (s_live_count > 0) {
        err = nvs_set_blob(h, STORE_LIVE, s_live, s_live_count * sizeof(sample_t));
    } else {
        err = nvs_erase_key(h, STORE_LIVE);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, STORE_VER, FEATURE_VERSION);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving the live samples failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t gem_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    esp_err_t err = nvs_flash_init_partition(STORE_PART);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition(STORE_PART));
        err = nvs_flash_init_partition(STORE_PART);
    }
    if (err != ESP_OK) {
        return err;
    }

    s_live = heap_caps_calloc(LIVE_MAX, sizeof(sample_t), MALLOC_CAP_SPIRAM);
    if (!s_live) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "AI model: %s v%d, %d stones", gem_ai_name(), gem_ai_version(), gem_ai_label_count());

    nvs_handle_t h;
    if (nvs_open_from_partition(STORE_PART, STORE_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "nothing stored yet");
        return ESP_OK;
    }
    uint8_t ver = 0;
    nvs_get_u8(h, STORE_VER, &ver);
    if (ver != 0 && ver != FEATURE_VERSION) {
        ESP_LOGW(TAG, "live samples are from feature version %u; please re-teach", ver);
    } else {
        size_t size = LIVE_MAX * sizeof(sample_t);
        if (nvs_get_blob(h, STORE_LIVE, s_live, &size) == ESP_OK) {
            s_live_count = size / sizeof(sample_t);
        }
    }
    uint8_t mode;
    if (nvs_get_u8(h, STORE_MODE, &mode) == ESP_OK && mode < GEM_MODE_COUNT) {
        s_mode = mode;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "%d live samples; using the %s mode", s_live_count, MODE_NAMES[s_mode]);
    return ESP_OK;
}

// ---- image decoding and features --------------------------------------------

// Decodes into s_rgb at 1/4 scale: a 640x480 frame becomes 160x120, enough for
// color statistics and more than the AI model's 96x96 input
static esp_err_t decode(const uint8_t *jpg, size_t len)
{
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpg,
        .indata_size = len,
        .out_format = JPEG_IMAGE_FORMAT_RGB888,
        .out_scale = JPEG_IMAGE_SCALE_1_4,
    };
    esp_jpeg_image_output_t img;
    esp_err_t err = esp_jpeg_get_image_info(&cfg, &img);
    if (err != ESP_OK) {
        return err;
    }
    if (img.output_len > s_rgb_size) {
        heap_caps_free(s_rgb);
        s_rgb = heap_caps_malloc(img.output_len, MALLOC_CAP_SPIRAM);
        s_rgb_size = s_rgb ? img.output_len : 0;
        if (!s_rgb) {
            return ESP_ERR_NO_MEM;
        }
    }
    cfg.outbuf = s_rgb;
    cfg.outbuf_size = s_rgb_size;
    err = esp_jpeg_decode(&cfg, &img);
    s_rgb_w = img.width;
    s_rgb_h = img.height;
    return err;
}

// Color/texture features of the middle half of s_rgb (the box on the web page)
static void features_from_rgb(gem_features_t *out)
{
    const int w = s_rgb_w, h = s_rgb_h;
    const int x0 = w / 4, x1 = w * 3 / 4, y0 = h / 4, y1 = h * 3 / 4;

    double sum[3] = {0}, sq[3] = {0}, sat_sum = 0;
    int hist[HUE_BINS] = {0}, gray = 0, n = 0;

    for (int y = y0; y < y1; y++) {
        const uint8_t *p = s_rgb + (y * w + x0) * 3;
        for (int x = x0; x < x1; x++, p += 3) {
            const int r = p[0], g = p[1], b = p[2];
            sum[0] += r; sum[1] += g; sum[2] += b;
            sq[0] += r * r; sq[1] += g * g; sq[2] += b * b;

            const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
            const int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
            const float sat = mx ? (float)(mx - mn) / mx : 0;
            sat_sum += sat;

            if (sat < 0.25f || mx < 30) {
                gray++;
            } else {
                const float d = mx - mn;
                float hue;  // 0..6
                if (mx == r) {
                    hue = (g - b) / d;
                    if (hue < 0) {
                        hue += 6;
                    }
                } else if (mx == g) {
                    hue = (b - r) / d + 2;
                } else {
                    hue = (r - g) / d + 4;
                }
                int bin = (int)(hue * HUE_BINS / 6);
                hist[bin >= HUE_BINS ? HUE_BINS - 1 : bin]++;
            }
            n++;
        }
    }

    for (int c = 0; c < 3; c++) {
        const double mean = sum[c] / n;
        const double var = sq[c] / n - mean * mean;
        out->f[c] = mean / 255.0;
        out->f[3 + c] = sqrt(var > 0 ? var : 0) / 128.0;
    }
    for (int i = 0; i < HUE_BINS; i++) {
        out->f[6 + i] = (float)hist[i] / n;
    }
    out->f[14] = (float)gray / n;
    out->f[15] = sat_sum / n;
}

esp_err_t gem_capture_features(int frames, gem_features_t *out)
{
    if (!gem_camera_ok()) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(out, 0, sizeof(*out));
    int got = 0;
    esp_err_t err = ESP_OK;

    gem_camera_lock();
    // Throw away a frame so we never use one taken before the stone was placed
    camera_fb_t *fb = gem_camera_grab();
    if (fb) {
        esp_camera_fb_return(fb);
    }
    for (int i = 0; i < frames; i++) {
        fb = gem_camera_grab();
        if (!fb) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        err = decode(fb->buf, fb->len);
        esp_camera_fb_return(fb);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "skipping a bad frame: %s", esp_err_to_name(err));
            continue;
        }
        gem_features_t one;
        features_from_rgb(&one);
        for (int k = 0; k < GEM_FEAT_N; k++) {
            out->f[k] += one.f[k];
        }
        got++;
    }
    gem_camera_unlock();

    if (got == 0) {
        return err != ESP_OK ? err : ESP_FAIL;
    }
    for (int k = 0; k < GEM_FEAT_N; k++) {
        out->f[k] /= got;
    }
    return ESP_OK;
}

// ---- mode -----------------------------------------------------------------------

const char *gem_mode_name(gem_mode_t m) { return MODE_NAMES[m]; }

bool gem_mode_from_name(const char *name, gem_mode_t *out)
{
    for (int m = 0; m < GEM_MODE_COUNT; m++) {
        if (strcmp(name, MODE_NAMES[m]) == 0) {
            *out = m;
            return true;
        }
    }
    return false;
}

gem_mode_t gem_mode(void) { return s_mode; }

esp_err_t gem_set_mode(gem_mode_t m)
{
    s_mode = m;
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(STORE_PART, STORE_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, STORE_MODE, m);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "now identifying with the %s mode", MODE_NAMES[m]);
    return err;
}

// ---- live demo samples ------------------------------------------------------------

bool gem_sanitize_label(char *label)
{
    // Keep letters, digits, space, '-' and '_'; collapse to lower case
    char *w = label;
    for (const char *r = label; *r && w - label < GEM_LABEL_LEN - 1; r++) {
        const char c = *r;
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || (c == ' ' && w > label && w[-1] != ' ')) {
            *w++ = tolower((unsigned char)c);
        }
    }
    while (w > label && w[-1] == ' ') {
        w--;
    }
    *w = '\0';
    return w > label;
}

esp_err_t gem_teach(const char *label, const gem_features_t *f)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (s_live_count < LIVE_MAX) {
        sample_t *s = &s_live[s_live_count++];
        strlcpy(s->label, label, sizeof(s->label));
        memcpy(s->f, f->f, sizeof(s->f));
        err = save_live();
        if (err != ESP_OK) {
            s_live_count--;
        }
    }
    const int n = s_live_count;
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "taught '%s' (%d live samples)", label, n);
    }
    return err;
}

esp_err_t gem_forget(const char *label)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int w = 0;
    for (int r = 0; r < s_live_count; r++) {
        if (label && strcmp(s_live[r].label, label) != 0) {
            s_live[w++] = s_live[r];
        }
    }
    ESP_LOGI(TAG, "forgot %d live samples of '%s'", s_live_count - w, label ? label : "(all)");
    s_live_count = w;
    esp_err_t err = save_live();
    xSemaphoreGive(s_lock);
    return err;
}

// ---- identify -------------------------------------------------------------------

static float distance(const float *a, const float *b)
{
    float d = 0;
    for (int i = 0; i < GEM_FEAT_N; i++) {
        const float x = (a[i] - b[i]) * WEIGHTS[i];
        d += x * x;
    }
    return sqrtf(d);
}

// Sorts out->scores highest first and fills in the winner and runner-up
static void finish_result(gem_result_t *out)
{
    for (int j = 1; j < out->nscores; j++) {
        const gem_score_t sc = out->scores[j];
        int k = j;
        while (k > 0 && out->scores[k - 1].score < sc.score) {
            out->scores[k] = out->scores[k - 1];
            k--;
        }
        out->scores[k] = sc;
    }
    strlcpy(out->label, out->scores[0].label, sizeof(out->label));
    out->confidence = out->scores[0].score;
    out->runner_up[0] = '\0';
    if (out->nscores > 1) {
        strlcpy(out->runner_up, out->scores[1].label, sizeof(out->runner_up));
    }
}

bool gem_identify_live(const gem_features_t *f, gem_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->mode = GEM_MODE_LIVE;

    struct {
        const char *label;
        float d[K_NEIGHBORS];  // closest first
        int n;
    } labs[GEM_MAX_LABELS];
    int nl = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_live_count; i++) {
        const char *lab = s_live[i].label;
        int j = 0;
        while (j < nl && strcmp(labs[j].label, lab) != 0) {
            j++;
        }
        if (j == nl) {
            if (nl == GEM_MAX_LABELS) {
                continue;  // more stones than the result can list
            }
            labs[nl].label = lab;
            labs[nl].n = 0;
            nl++;
        }
        // Keep this label's K closest distances, sorted
        const float d = distance(f->f, s_live[i].f);
        int pos = labs[j].n < K_NEIGHBORS ? labs[j].n++ : K_NEIGHBORS;
        while (pos > 0 && labs[j].d[pos - 1] > d) {
            if (pos < K_NEIGHBORS) {
                labs[j].d[pos] = labs[j].d[pos - 1];
            }
            pos--;
        }
        if (pos < K_NEIGHBORS) {
            labs[j].d[pos] = d;
        }
    }

    // Closer labels get a bigger share: weight = 1 / distance^2, normalized to 100%
    float total = 0;
    for (int j = 0; j < nl; j++) {
        float sum = 0;
        for (int k = 0; k < labs[j].n; k++) {
            sum += labs[j].d[k];
        }
        const float dist = sum / labs[j].n;
        out->scores[j].score = 1.0f / (dist * dist + 1e-4f);
        total += out->scores[j].score;
        strlcpy(out->scores[j].label, labs[j].label, GEM_LABEL_LEN);
    }
    xSemaphoreGive(s_lock);

    if (nl == 0) {
        return false;
    }
    for (int j = 0; j < nl; j++) {
        out->scores[j].score /= total;
    }
    out->nscores = nl;
    finish_result(out);
    return true;
}

static void fill_trained_result(const float *scores, gem_result_t *out)
{
    const int n = gem_ai_label_count() < GEM_MAX_LABELS ? gem_ai_label_count() : GEM_MAX_LABELS;
    for (int i = 0; i < n; i++) {
        strlcpy(out->scores[i].label, gem_ai_label(i), GEM_LABEL_LEN);
        out->scores[i].score = scores[i];
    }
    out->nscores = n;
    finish_result(out);
}

static esp_err_t identify_trained(gem_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->mode = GEM_MODE_TRAINED;
    if (!gem_camera_ok()) {
        return ESP_ERR_INVALID_STATE;
    }
    float scores[GEM_MAX_LABELS];
    int ai_ms = 0;

    gem_camera_lock();
    camera_fb_t *fb = gem_camera_grab();  // stale frame, see gem_capture_features()
    if (fb) {
        esp_camera_fb_return(fb);
    }
    fb = gem_camera_grab();
    esp_err_t err = fb ? decode(fb->buf, fb->len) : ESP_ERR_TIMEOUT;
    if (fb) {
        esp_camera_fb_return(fb);
    }
    if (err == ESP_OK) {
        err = gem_ai_classify(s_rgb, s_rgb_w, s_rgb_h, scores, &ai_ms);
    }
    gem_camera_unlock();
    if (err != ESP_OK) {
        return err;
    }
    fill_trained_result(scores, out);
    ESP_LOGI(TAG, "inference took %d ms", ai_ms);
    return ESP_OK;
}

esp_err_t gem_classify_jpeg(const uint8_t *jpg, size_t len, gem_result_t *out,
                            uint8_t *input, size_t input_size, int *input_w, int *input_h)
{
    memset(out, 0, sizeof(*out));
    out->mode = GEM_MODE_TRAINED;
    float scores[GEM_MAX_LABELS];
    int ai_ms = 0;
    const int64_t t0 = esp_timer_get_time();

    // s_rgb is shared with the camera path, so this runs under the same lock
    gem_camera_lock();
    esp_err_t err = decode(jpg, len);
    if (err == ESP_OK) {
        err = gem_ai_classify(s_rgb, s_rgb_w, s_rgb_h, scores, &ai_ms);
    }
    if (err == ESP_OK && input) {
        int w, h;
        const uint8_t *in = gem_ai_last_input(&w, &h);
        const size_t n = (size_t)w * h * 3;
        if (!in || n > input_size) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(input, in, n);
            *input_w = w;
            *input_h = h;
        }
    }
    const int dw = s_rgb_w, dh = s_rgb_h;
    gem_camera_unlock();
    if (err != ESP_OK) {
        return err;
    }
    fill_trained_result(scores, out);
    out->ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "uploaded %u-byte JPEG decoded to %dx%d: %s %.0f%% (%d ms inference)",
             (unsigned)len, dw, dh, out->label, out->confidence * 100, ai_ms);
    return ESP_OK;
}

void gem_ai_warmup(void)
{
    gem_result_t *r = malloc(sizeof(*r));
    if (!r) {
        return;
    }
    // Twice: the first run sets things up, the second shows the normal speed
    for (int i = 0; i < 2; i++) {
        const int64_t t0 = esp_timer_get_time();
        esp_err_t err = identify_trained(r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AI model test run failed: %s", esp_err_to_name(err));
            break;
        }
        ESP_LOGI(TAG, "AI model test %d: sees '%s' (%.0f%%) in %lld ms", i + 1,
                 r->label, r->confidence * 100, (esp_timer_get_time() - t0) / 1000);
    }
    free(r);
}

esp_err_t gem_identify_now(gem_result_t *out)
{
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err;
    if (s_mode == GEM_MODE_TRAINED) {
        err = identify_trained(out);
    } else {
        gem_features_t f;
        err = gem_capture_features(3, &f);
        if (err == ESP_OK && !gem_identify_live(&f, out)) {
            err = ESP_ERR_NOT_FOUND;
        }
    }
    if (err != ESP_OK) {
        return err;
    }
    out->ms = (esp_timer_get_time() - t0) / 1000;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->seq = ++s_seq;
    s_last = *out;
    xSemaphoreGive(s_lock);

    gem_show_result(out);
    return ESP_OK;
}

bool gem_last_result(gem_result_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_last;
    xSemaphoreGive(s_lock);
    return out->seq != 0;
}

unsigned gem_result_seq(void) { return s_seq; }

// ---- reporting ----------------------------------------------------------------

int gem_live_count(void) { return s_live_count; }

int gem_live_labels_json(char *buf, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = snprintf(buf, len, "[");
    for (int i = 0; i < s_live_count && n < (int)len; i++) {
        // Count each label once, at its first occurrence
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) {
            seen = strcmp(s_live[j].label, s_live[i].label) == 0;
        }
        if (seen) {
            continue;
        }
        int count = 0;
        for (int j = i; j < s_live_count; j++) {
            count += strcmp(s_live[j].label, s_live[i].label) == 0;
        }
        // Labels are sanitized, so they need no JSON escaping
        n += snprintf(buf + n, len - n, "%s{\"label\":\"%s\",\"count\":%d}",
                      n > 1 ? "," : "", s_live[i].label, count);
    }
    xSemaphoreGive(s_lock);
    if (n < (int)len) {
        n += snprintf(buf + n, len - n, "]");
    }
    return n;
}

int gem_ai_info_json(char *buf, size_t len)
{
    // Edge Impulse labels are plain names; no JSON escaping needed
    int n = snprintf(buf, len, "{\"name\":\"%s\",\"version\":%d,\"labels\":[",
                     gem_ai_name(), gem_ai_version());
    for (int i = 0; i < gem_ai_label_count() && n < (int)len; i++) {
        n += snprintf(buf + n, len - n, "%s{\"label\":\"%s\"}", i ? "," : "", gem_ai_label(i));
    }
    if (n < (int)len) {
        n += snprintf(buf + n, len - n, "]}");
    }
    return n;
}

int gem_result_json(const gem_result_t *r, char *buf, size_t len)
{
    int n = snprintf(buf, len,
                     "{\"seq\":%u,\"mode\":\"%s\",\"label\":\"%s\",\"confidence\":%.3f,"
                     "\"runner_up\":\"%s\",\"ms\":%d,\"scores\":[",
                     r->seq, MODE_NAMES[r->mode], r->label, r->confidence, r->runner_up, r->ms);
    for (int i = 0; i < r->nscores && n < (int)len; i++) {
        n += snprintf(buf + n, len - n, "%s{\"label\":\"%s\",\"score\":%.3f}",
                      i ? "," : "", r->scores[i].label, r->scores[i].score);
    }
    if (n < (int)len) {
        n += snprintf(buf + n, len - n, "]}");
    }
    return n;
}

void gem_show_result(const gem_result_t *r)
{
    // TODO: also draw this on the SSD1306 once it's wired up
    ESP_LOGI(TAG, "RESULT: %s  %.0f%%  (%s%s%s, %d ms)",
             r->label, r->confidence * 100, MODE_NAMES[r->mode],
             r->runner_up[0] ? ", next: " : "", r->runner_up, r->ms);
}
