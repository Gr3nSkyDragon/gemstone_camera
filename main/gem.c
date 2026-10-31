// Colour/texture features + nearest-neighbour matching against stored samples.
//
// This is the "classic computer vision" baseline: no neural network, it just
// measures the colours in the middle of the picture and finds the most similar
// stones it has been shown.
//
// Samples live in two banks so a live demo can't disturb the real training:
//   trained  filled from a photo dataset by tools/train.py (dozens per stone)
//   live     taught from the web page a few presses at a time; wipe at will

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
#define STORE_VER  "fver"
#define STORE_MODE "mode"
// Bump when the feature layout changes; old samples are then discarded
#define FEATURE_VERSION 1

// Each label's distance is the mean of its K closest samples
#define K_NEIGHBOURS 3
#define HUE_BINS 8

typedef struct {
    char label[GEM_LABEL_LEN];
    float f[GEM_FEAT_N];
} sample_t;

typedef struct {
    const char *name;  // shown on the web page and used in the API
    const char *key;   // NVS key
    int max;
    sample_t *s;
    int count;
} bank_t;

static bank_t s_banks[GEM_BANK_COUNT] = {
    [GEM_BANK_TRAINED] = {.name = "trained", .key = "trained", .max = 1000},
    // "samples" is the key the single-bank alpha used, so earlier teaching is kept
    [GEM_BANK_LIVE]    = {.name = "live",    .key = "samples", .max = 300},
};

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

static SemaphoreHandle_t s_lock;   // guards the banks, mode and last result
static gem_bank_t s_mode = GEM_BANK_LIVE;
static gem_result_t s_last;
static unsigned s_seq;

static uint8_t *s_rgb;             // decode buffer; only used under the camera lock
static size_t s_rgb_size;

// ---- storage ----------------------------------------------------------------

static esp_err_t save_bank(gem_bank_t b)
{
    const bank_t *bk = &s_banks[b];
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(STORE_PART, STORE_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (bk->count > 0) {
        err = nvs_set_blob(h, bk->key, bk->s, bk->count * sizeof(sample_t));
    } else {
        err = nvs_erase_key(h, bk->key);
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
        ESP_LOGE(TAG, "saving the %s samples failed: %s", bk->name, esp_err_to_name(err));
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

    for (int b = 0; b < GEM_BANK_COUNT; b++) {
        s_banks[b].s = heap_caps_calloc(s_banks[b].max, sizeof(sample_t), MALLOC_CAP_SPIRAM);
        if (!s_banks[b].s) {
            return ESP_ERR_NO_MEM;
        }
    }

    nvs_handle_t h;
    if (nvs_open_from_partition(STORE_PART, STORE_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "nothing stored yet");
        return ESP_OK;
    }
    uint8_t ver = 0;
    nvs_get_u8(h, STORE_VER, &ver);
    if (ver != 0 && ver != FEATURE_VERSION) {
        ESP_LOGW(TAG, "samples are from feature version %u; please re-teach", ver);
    } else {
        for (int b = 0; b < GEM_BANK_COUNT; b++) {
            bank_t *bk = &s_banks[b];
            size_t size = bk->max * sizeof(sample_t);
            if (nvs_get_blob(h, bk->key, bk->s, &size) == ESP_OK) {
                bk->count = size / sizeof(sample_t);
            }
        }
    }
    uint8_t mode;
    if (nvs_get_u8(h, STORE_MODE, &mode) == ESP_OK && mode < GEM_BANK_COUNT) {
        s_mode = mode;
    } else if (s_banks[GEM_BANK_TRAINED].count > 0) {
        s_mode = GEM_BANK_TRAINED;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded %d trained and %d live samples; using %s",
             s_banks[GEM_BANK_TRAINED].count, s_banks[GEM_BANK_LIVE].count, s_banks[s_mode].name);
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

// ---- banks and mode -----------------------------------------------------------

const char *gem_bank_name(gem_bank_t b) { return s_banks[b].name; }

bool gem_bank_from_name(const char *name, gem_bank_t *out)
{
    for (int b = 0; b < GEM_BANK_COUNT; b++) {
        if (strcmp(name, s_banks[b].name) == 0) {
            *out = b;
            return true;
        }
    }
    return false;
}

gem_bank_t gem_mode(void) { return s_mode; }

esp_err_t gem_set_mode(gem_bank_t b)
{
    s_mode = b;
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(STORE_PART, STORE_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, STORE_MODE, b);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "now identifying with the %s samples", s_banks[b].name);
    return err;
}

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

static esp_err_t add_sample(gem_bank_t b, const char *label, const gem_features_t *f)
{
    bank_t *bk = &s_banks[b];
    if (bk->count >= bk->max) {
        return ESP_ERR_NO_MEM;
    }
    sample_t *s = &bk->s[bk->count];
    strlcpy(s->label, label, sizeof(s->label));
    memcpy(s->f, f->f, sizeof(s->f));
    bk->count++;
    return ESP_OK;
}

esp_err_t gem_teach(const char *label, const gem_features_t *f)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = add_sample(GEM_BANK_LIVE, label, f);
    if (err == ESP_OK) {
        err = save_bank(GEM_BANK_LIVE);
        if (err != ESP_OK) {
            s_banks[GEM_BANK_LIVE].count--;
        }
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "taught '%s' (%d live samples)", label, s_banks[GEM_BANK_LIVE].count);
    }
    return err;
}

esp_err_t gem_forget(const char *label)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bank_t *bk = &s_banks[GEM_BANK_LIVE];
    int w = 0;
    for (int r = 0; r < bk->count; r++) {
        if (label && strcmp(bk->s[r].label, label) != 0) {
            bk->s[w++] = bk->s[r];
        }
    }
    ESP_LOGI(TAG, "forgot %d live samples of '%s'", bk->count - w, label ? label : "(all)");
    bk->count = w;
    esp_err_t err = save_bank(GEM_BANK_LIVE);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t gem_train_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_banks[GEM_BANK_TRAINED].count = 0;
    esp_err_t err = save_bank(GEM_BANK_TRAINED);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "trained samples cleared");
    return err;
}

esp_err_t gem_train_add_jpeg(const char *label, const uint8_t *jpg, size_t len)
{
    gem_features_t f;
    gem_camera_lock();  // the decode buffer is shared with the camera path
    esp_err_t err = features_from_jpeg(jpg, len, &f);
    gem_camera_unlock();
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = add_sample(GEM_BANK_TRAINED, label, &f);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t gem_train_commit(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = save_bank(GEM_BANK_TRAINED);
    const int n = s_banks[GEM_BANK_TRAINED].count;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "saved %d trained samples", n);
    return err;
}

// ---- identify -----------------------------------------------------------------

static float distance(const float *a, const float *b)
{
    float d = 0;
    for (int i = 0; i < GEM_FEAT_N; i++) {
        const float x = (a[i] - b[i]) * WEIGHTS[i];
        d += x * x;
    }
    return sqrtf(d);
}

bool gem_identify(gem_bank_t b, const gem_features_t *f, gem_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->bank = b;

    struct {
        const char *label;
        float d[K_NEIGHBOURS];  // closest first
        int n;
    } labs[GEM_MAX_LABELS];
    int nl = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bank_t *bk = &s_banks[b];
    for (int i = 0; i < bk->count; i++) {
        const char *lab = bk->s[i].label;
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
        const float d = distance(f->f, bk->s[i].f);
        int pos = labs[j].n < K_NEIGHBOURS ? labs[j].n++ : K_NEIGHBOURS;
        while (pos > 0 && labs[j].d[pos - 1] > d) {
            if (pos < K_NEIGHBOURS) {
                labs[j].d[pos] = labs[j].d[pos - 1];
            }
            pos--;
        }
        if (pos < K_NEIGHBOURS) {
            labs[j].d[pos] = d;
        }
    }

    if (nl == 0) {
        xSemaphoreGive(s_lock);
        return false;
    }

    // Closer labels get a bigger share: weight = 1 / distance^2, normalised to 100%
    float dist[GEM_MAX_LABELS], total = 0;
    for (int j = 0; j < nl; j++) {
        float sum = 0;
        for (int k = 0; k < labs[j].n; k++) {
            sum += labs[j].d[k];
        }
        dist[j] = sum / labs[j].n;
        out->scores[j].score = 1.0f / (dist[j] * dist[j] + 1e-4f);
        total += out->scores[j].score;
        strlcpy(out->scores[j].label, labs[j].label, GEM_LABEL_LEN);
    }
    xSemaphoreGive(s_lock);

    for (int j = 0; j < nl; j++) {
        out->scores[j].score /= total;
    }
    // Highest score first (insertion sort; there are only a handful of labels)
    for (int j = 1; j < nl; j++) {
        const gem_score_t sc = out->scores[j];
        const float dj = dist[j];
        int k = j;
        while (k > 0 && out->scores[k - 1].score < sc.score) {
            out->scores[k] = out->scores[k - 1];
            dist[k] = dist[k - 1];
            k--;
        }
        out->scores[k] = sc;
        dist[k] = dj;
    }

    out->nscores = nl;
    strlcpy(out->label, out->scores[0].label, sizeof(out->label));
    out->confidence = out->scores[0].score;
    out->distance = dist[0];
    if (nl > 1) {
        strlcpy(out->runner_up, out->scores[1].label, sizeof(out->runner_up));
    }
    return true;
}

esp_err_t gem_identify_now(gem_result_t *out)
{
    const int64_t t0 = esp_timer_get_time();
    gem_features_t f;
    esp_err_t err = gem_capture_features(3, &f);
    if (err != ESP_OK) {
        return err;
    }
    if (!gem_identify(s_mode, &f, out)) {
        return ESP_ERR_NOT_FOUND;
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

int gem_sample_count(gem_bank_t b) { return s_banks[b].count; }

int gem_labels_json(gem_bank_t b, char *buf, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bank_t *bk = &s_banks[b];
    int n = snprintf(buf, len, "[");
    for (int i = 0; i < bk->count && n < (int)len; i++) {
        // Count each label once, at its first occurrence
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) {
            seen = strcmp(bk->s[j].label, bk->s[i].label) == 0;
        }
        if (seen) {
            continue;
        }
        int count = 0;
        for (int j = i; j < bk->count; j++) {
            count += strcmp(bk->s[j].label, bk->s[i].label) == 0;
        }
        // Labels are sanitized, so they need no JSON escaping
        n += snprintf(buf + n, len - n, "%s{\"label\":\"%s\",\"count\":%d}",
                      n > 1 ? "," : "", bk->s[i].label, count);
    }
    xSemaphoreGive(s_lock);
    if (n < (int)len) {
        n += snprintf(buf + n, len - n, "]");
    }
    return n;
}

int gem_result_json(const gem_result_t *r, char *buf, size_t len)
{
    int n = snprintf(buf, len,
                     "{\"seq\":%u,\"mode\":\"%s\",\"label\":\"%s\",\"confidence\":%.3f,"
                     "\"distance\":%.4f,\"runner_up\":\"%s\",\"ms\":%d,\"scores\":[",
                     r->seq, s_banks[r->bank].name, r->label, r->confidence,
                     r->distance, r->runner_up, r->ms);
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
    ESP_LOGI(TAG, "RESULT: %s  %.0f%%  (%s samples, distance %.3f%s%s, %d ms)",
             r->label, r->confidence * 100, s_banks[r->bank].name, r->distance,
             r->runner_up[0] ? ", next: " : "", r->runner_up, r->ms);
}
