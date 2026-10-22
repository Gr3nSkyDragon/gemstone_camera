// Colour/texture features + k-nearest-neighbour matching against taught samples.
//
// This is the "classic computer vision" baseline: no neural network, it just
// measures the colours in the middle of the picture and finds the most similar
// stones it has been shown. A trained CNN can replace gem_identify() later.

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "gemcam.h"

static const char *TAG = "gem";

#define STORE_PART "gemdata"
#define STORE_NS   "gems"
#define STORE_KEY  "samples"
#define STORE_VER  "fver"
// Bump when the feature layout changes; old samples are then discarded
#define FEATURE_VERSION 1

#define K_NEIGHBOURS 3
#define HUE_BINS 8

typedef struct {
    char label[GEM_LABEL_LEN];
    float f[GEM_FEAT_N];
} sample_t;

// Feature layout (all roughly 0..1):
//   0-2   mean R, G, B
//   3-5   standard deviation of R, G, B  (texture, banding, sparkle)
//   6-13  hue histogram of coloured pixels, 8 bins of 45 degrees
//   14    fraction of grey/white/black pixels
//   15    mean saturation
static const float WEIGHTS[GEM_FEAT_N] = {
    2, 2, 2,
    1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1,
    1,
};

static sample_t *s_samples;
static int s_count;
static uint8_t *s_rgb;
static size_t s_rgb_size;

// ---- storage ----------------------------------------------------------------

static esp_err_t save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(STORE_PART, STORE_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (s_count > 0) {
        err = nvs_set_blob(h, STORE_KEY, s_samples, s_count * sizeof(sample_t));
    } else {
        err = nvs_erase_key(h, STORE_KEY);
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
        ESP_LOGE(TAG, "saving samples failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t gem_store_init(void)
{
    esp_err_t err = nvs_flash_init_partition(STORE_PART);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition(STORE_PART));
        err = nvs_flash_init_partition(STORE_PART);
    }
    if (err != ESP_OK) {
        return err;
    }

    s_samples = heap_caps_calloc(GEM_MAX_SAMPLES, sizeof(sample_t), MALLOC_CAP_SPIRAM);
    if (!s_samples) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t h;
    if (nvs_open_from_partition(STORE_PART, STORE_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no taught samples yet");
        return ESP_OK;
    }
    uint8_t ver = 0;
    nvs_get_u8(h, STORE_VER, &ver);
    size_t size = GEM_MAX_SAMPLES * sizeof(sample_t);
    if (ver == FEATURE_VERSION && nvs_get_blob(h, STORE_KEY, s_samples, &size) == ESP_OK) {
        s_count = size / sizeof(sample_t);
    } else if (ver != 0 && ver != FEATURE_VERSION) {
        ESP_LOGW(TAG, "samples are from feature version %u; please re-teach", ver);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded %d taught samples", s_count);
    return ESP_OK;
}

// ---- features ---------------------------------------------------------------

static esp_err_t features_from_jpeg(const uint8_t *jpg, size_t len, gem_features_t *out)
{
    // 1/4 scale: a 640x480 frame decodes to 160x120, plenty for colour statistics
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
    if (err != ESP_OK) {
        return err;
    }

    // Only the middle half of the picture (the box drawn on the web page)
    const int w = img.width, h = img.height;
    const int x0 = w / 4, x1 = w * 3 / 4, y0 = h / 4, y1 = h * 3 / 4;

    double sum[3] = {0}, sq[3] = {0}, sat_sum = 0;
    int hist[HUE_BINS] = {0}, grey = 0, n = 0;

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
                grey++;
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
    out->f[14] = (float)grey / n;
    out->f[15] = sat_sum / n;
    return ESP_OK;
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
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        esp_camera_fb_return(fb);
    }
    for (int i = 0; i < frames; i++) {
        fb = esp_camera_fb_get();
        if (!fb) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        gem_features_t one;
        err = features_from_jpeg(fb->buf, fb->len, &one);
        esp_camera_fb_return(fb);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "skipping a bad frame: %s", esp_err_to_name(err));
            continue;
        }
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

// ---- teach / identify -------------------------------------------------------

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
    if (s_count >= GEM_MAX_SAMPLES) {
        return ESP_ERR_NO_MEM;
    }
    sample_t *s = &s_samples[s_count];
    strlcpy(s->label, label, sizeof(s->label));
    memcpy(s->f, f->f, sizeof(s->f));
    s_count++;
    ESP_LOGI(TAG, "taught '%s' (%d samples total)", label, s_count);
    esp_err_t err = save();
    if (err != ESP_OK) {
        s_count--;
    }
    return err;
}

static float distance(const float *a, const float *b)
{
    float d = 0;
    for (int i = 0; i < GEM_FEAT_N; i++) {
        const float x = (a[i] - b[i]) * WEIGHTS[i];
        d += x * x;
    }
    return sqrtf(d);
}

bool gem_identify(const gem_features_t *f, gem_result_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_count == 0) {
        return false;
    }

    // k nearest samples, closest first
    int best[K_NEIGHBOURS];
    float best_d[K_NEIGHBOURS];
    int k = 0;
    for (int i = 0; i < s_count; i++) {
        const float d = distance(f->f, s_samples[i].f);
        int pos = k < K_NEIGHBOURS ? k++ : K_NEIGHBOURS;
        while (pos > 0 && best_d[pos - 1] > d) {
            if (pos < K_NEIGHBOURS) {
                best[pos] = best[pos - 1];
                best_d[pos] = best_d[pos - 1];
            }
            pos--;
        }
        if (pos < K_NEIGHBOURS) {
            best[pos] = i;
            best_d[pos] = d;
        }
    }

    // Closer neighbours get a bigger vote
    const char *labels[K_NEIGHBOURS];
    float votes[K_NEIGHBOURS], nearest[K_NEIGHBOURS], total = 0;
    int nl = 0;
    for (int i = 0; i < k; i++) {
        const char *lab = s_samples[best[i]].label;
        const float v = 1.0f / (best_d[i] + 0.01f);
        total += v;
        int j = 0;
        while (j < nl && strcmp(labels[j], lab) != 0) {
            j++;
        }
        if (j == nl) {
            labels[nl] = lab;
            votes[nl] = 0;
            nearest[nl] = best_d[i];
            nl++;
        }
        votes[j] += v;
    }

    int win = 0, second = -1;
    for (int j = 1; j < nl; j++) {
        if (votes[j] > votes[win]) {
            second = win;
            win = j;
        } else if (second < 0 || votes[j] > votes[second]) {
            second = j;
        }
    }

    strlcpy(out->label, labels[win], sizeof(out->label));
    out->confidence = votes[win] / total;
    out->distance = nearest[win];
    if (second >= 0) {
        strlcpy(out->runner_up, labels[second], sizeof(out->runner_up));
    }
    return true;
}

esp_err_t gem_forget(const char *label)
{
    int w = 0;
    for (int r = 0; r < s_count; r++) {
        if (label && strcmp(s_samples[r].label, label) != 0) {
            s_samples[w++] = s_samples[r];
        }
    }
    ESP_LOGI(TAG, "forgot %d samples of '%s'", s_count - w, label ? label : "(all)");
    s_count = w;
    return save();
}

int gem_sample_count(void) { return s_count; }

int gem_labels_json(char *buf, size_t len)
{
    int n = snprintf(buf, len, "[");
    for (int i = 0; i < s_count && n < (int)len; i++) {
        // Count each label once, at its first occurrence
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) {
            seen = strcmp(s_samples[j].label, s_samples[i].label) == 0;
        }
        if (seen) {
            continue;
        }
        int count = 0;
        for (int j = i; j < s_count; j++) {
            count += strcmp(s_samples[j].label, s_samples[i].label) == 0;
        }
        // Labels are sanitized, so they need no JSON escaping
        n += snprintf(buf + n, len - n, "%s{\"label\":\"%s\",\"count\":%d}",
                      n > 1 ? "," : "", s_samples[i].label, count);
    }
    if (n < (int)len) {
        n += snprintf(buf + n, len - n, "]");
    }
    return n;
}

void gem_show_result(const gem_result_t *r)
{
    // TODO: also draw this on the SSD1306 once it's wired up
    ESP_LOGI(TAG, "RESULT: %s  %.0f%%  (distance %.3f%s%s, %d ms)",
             r->label, r->confidence * 100, r->distance,
             r->runner_up[0] ? ", next: " : "", r->runner_up, r->ms);
}

esp_err_t gem_identify_now(gem_result_t *out)
{
    const int64_t t0 = esp_timer_get_time();
    gem_features_t f;
    esp_err_t err = gem_capture_features(3, &f);
    if (err != ESP_OK) {
        return err;
    }
    if (!gem_identify(&f, out)) {
        return ESP_ERR_NOT_FOUND;
    }
    out->ms = (esp_timer_get_time() - t0) / 1000;
    gem_show_result(out);
    return ESP_OK;
}
