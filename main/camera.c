#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "gemcam.h"

static const char *TAG = "camera";

// Same wiring as the ESP32-S3-EYE, so stock Arduino camera examples also work
// with CAMERA_MODEL_ESP32S3_EYE. PWDN is tied to GND and RESET to 3.3V.
// ALIENTEK-style modules have their own oscillator, so XCLK defaults to unused.
#define PIN_XCLK  CONFIG_GEMCAM_XCLK_GPIO
#define PIN_SIOD  4
#define PIN_SIOC  5
#define PIN_D7    16  // Y9
#define PIN_D6    17  // Y8
#define PIN_D5    18  // Y7
#define PIN_D4    12  // Y6
#define PIN_D3    10  // Y5
#define PIN_D2    8   // Y4
#define PIN_D1    9   // Y3
#define PIN_D0    11  // Y2
#define PIN_VSYNC 6
#define PIN_HREF  7
#define PIN_PCLK  13

#define SETTINGS_PART "gemdata"
#define SETTINGS_NS   "cam"

typedef enum {
    CTL_BRIGHTNESS, CTL_CONTRAST, CTL_SATURATION, CTL_AE_LEVEL,
    CTL_AWB, CTL_WB_MODE, CTL_AEC, CTL_AEC_VALUE, CTL_AGC, CTL_AGC_GAIN,
    CTL_VFLIP, CTL_HMIRROR, CTL_QUALITY, CTL_COUNT
} control_id_t;

static const struct {
    const char *name;
    int min, max;
} CONTROLS[CTL_COUNT] = {
    [CTL_BRIGHTNESS] = {"brightness", -2, 2},
    [CTL_CONTRAST]   = {"contrast",   -2, 2},
    [CTL_SATURATION] = {"saturation", -2, 2},
    [CTL_AE_LEVEL]   = {"ae_level",   -2, 2},
    [CTL_AWB]        = {"awb",         0, 1},
    [CTL_WB_MODE]    = {"wb_mode",     0, 4},
    [CTL_AEC]        = {"aec",         0, 1},
    [CTL_AEC_VALUE]  = {"aec_value",   0, 1200},
    [CTL_AGC]        = {"agc",         0, 1},
    [CTL_AGC_GAIN]   = {"agc_gain",    0, 30},
    [CTL_VFLIP]      = {"vflip",       0, 1},
    [CTL_HMIRROR]    = {"hmirror",     0, 1},
    [CTL_QUALITY]    = {"quality",     4, 40},
};

static SemaphoreHandle_t s_lock;
static bool s_ok;
static char s_error[256] = "Camera not started";
static int s_values[CTL_COUNT];

// ---- wiring diagnosis (runs only when the camera fails to start) -------------

// 0 = held low, 1 = pulled high by the module, 2 = floating (no pull-up seen)
static int line_state(int pin)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(5));
    if (gpio_get_level(pin)) {
        return 1;
    }
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(5));
    const int up = gpio_get_level(pin);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    return up ? 2 : 0;
}

static const char *line_text(int st)
{
    return st == 1 ? "pulled high by the module (good)"
         : st == 2 ? "floating - no pull-up resistor seen (some modules have none; else check the wire)"
         : "held LOW - shorted to GND, or the module has no power";
}

static void diagnose_sccb(void)
{
    const int sda = line_state(PIN_SIOD), scl = line_state(PIN_SIOC);
    ESP_LOGW(TAG, "diag: SDA (GPIO %d) is %s", PIN_SIOD, line_text(sda));
    ESP_LOGW(TAG, "diag: SCL (GPIO %d) is %s", PIN_SIOC, line_text(scl));

    const i2c_port_t port = I2C_NUM_1;
    const i2c_config_t c = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_SIOD,
        .scl_io_num = PIN_SIOC,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 50000,
    };
    if (i2c_param_config(port, &c) != ESP_OK || i2c_driver_install(port, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) {
        ESP_LOGW(TAG, "diag: couldn't start I2C for the scan");
        return;
    }

    char found[96] = "";
    int nfound = 0;
    for (int addr = 0x08; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, addr << 1, true);
        i2c_master_stop(cmd);
        const esp_err_t r = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);
        if (r == ESP_OK && nfound < 12) {
            snprintf(found + strlen(found), sizeof(found) - strlen(found), " 0x%02x", addr);
            nfound++;
        }
    }
    ESP_LOGW(TAG, "diag: devices answering on SDA/SCL:%s", nfound ? found : " none");

    // OV2640 lives at 0x30; select the sensor register bank and read its ID
    int pid = -1, ver = -1;
    if (strstr(found, "0x30")) {
        const uint8_t bank[2] = {0xff, 0x01};
        uint8_t reg, v;
        if (i2c_master_write_to_device(port, 0x30, bank, 2, pdMS_TO_TICKS(50)) == ESP_OK) {
            reg = 0x0a;
            if (i2c_master_write_to_device(port, 0x30, &reg, 1, pdMS_TO_TICKS(50)) == ESP_OK &&
                i2c_master_read_from_device(port, 0x30, &v, 1, pdMS_TO_TICKS(50)) == ESP_OK) {
                pid = v;
            }
            reg = 0x0b;
            if (i2c_master_write_to_device(port, 0x30, &reg, 1, pdMS_TO_TICKS(50)) == ESP_OK &&
                i2c_master_read_from_device(port, 0x30, &v, 1, pdMS_TO_TICKS(50)) == ESP_OK) {
                ver = v;
            }
        }
        ESP_LOGW(TAG, "diag: device at 0x30 reports PID 0x%02x VER 0x%02x (an OV2640 is 0x26 0x4x)", pid, ver);
    }
    i2c_driver_delete(port);

    if (nfound == 0) {
        snprintf(s_error, sizeof(s_error),
                 "Camera not answering on SDA/SCL (SDA %s, SCL %s). Check those two wires and 3.3V/GND; "
                 "if they're right, the module may need a clock on XCLK.",
                 sda == 1 ? "pulled up" : sda == 2 ? "floating" : "held low",
                 scl == 1 ? "pulled up" : scl == 2 ? "floating" : "held low");
    } else {
        snprintf(s_error, sizeof(s_error),
                 "Something answers on SDA/SCL (%s) but it isn't a supported camera "
                 "(PID 0x%02x). See the serial log.", found + 1, pid);
    }
}

static int apply(sensor_t *s, control_id_t id, int v)
{
    switch (id) {
    case CTL_BRIGHTNESS: return s->set_brightness(s, v);
    case CTL_CONTRAST:   return s->set_contrast(s, v);
    case CTL_SATURATION: return s->set_saturation(s, v);
    case CTL_AE_LEVEL:   return s->set_ae_level(s, v);
    case CTL_AWB:        return s->set_whitebal(s, v);
    case CTL_WB_MODE:    return s->set_wb_mode(s, v);
    case CTL_AEC:        return s->set_exposure_ctrl(s, v);
    case CTL_AEC_VALUE:  return s->set_aec_value(s, v);
    case CTL_AGC:        return s->set_gain_ctrl(s, v);
    case CTL_AGC_GAIN:   return s->set_agc_gain(s, v);
    case CTL_VFLIP:      return s->set_vflip(s, v);
    case CTL_HMIRROR:    return s->set_hmirror(s, v);
    case CTL_QUALITY:    return s->set_quality(s, v);
    default:             return -1;
    }
}

static void read_status(sensor_t *s)
{
    const camera_status_t *st = &s->status;
    s_values[CTL_BRIGHTNESS] = st->brightness;
    s_values[CTL_CONTRAST]   = st->contrast;
    s_values[CTL_SATURATION] = st->saturation;
    s_values[CTL_AE_LEVEL]   = st->ae_level;
    s_values[CTL_AWB]        = st->awb;
    s_values[CTL_WB_MODE]    = st->wb_mode;
    s_values[CTL_AEC]        = st->aec;
    s_values[CTL_AEC_VALUE]  = st->aec_value;
    s_values[CTL_AGC]        = st->agc;
    s_values[CTL_AGC_GAIN]   = st->agc_gain;
    s_values[CTL_VFLIP]      = st->vflip;
    s_values[CTL_HMIRROR]    = st->hmirror;
    s_values[CTL_QUALITY]    = st->quality;
}

// Re-applies settings from the last session so taught samples stay comparable
static void load_saved(sensor_t *s)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(SETTINGS_PART, SETTINGS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    for (int i = 0; i < CTL_COUNT; i++) {
        int16_t v;
        if (nvs_get_i16(h, CONTROLS[i].name, &v) == ESP_OK) {
            apply(s, i, v);
            s_values[i] = v;
            ESP_LOGI(TAG, "restored %s = %d", CONTROLS[i].name, v);
        }
    }
    nvs_close(h);
}

esp_err_t gem_camera_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    camera_config_t cfg = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = PIN_XCLK,
        .pin_sccb_sda = PIN_SIOD,
        .pin_sccb_scl = PIN_SIOC,
        .pin_d7 = PIN_D7, .pin_d6 = PIN_D6, .pin_d5 = PIN_D5, .pin_d4 = PIN_D4,
        .pin_d3 = PIN_D3, .pin_d2 = PIN_D2, .pin_d1 = PIN_D1, .pin_d0 = PIN_D0,
        .pin_vsync = PIN_VSYNC,
        .pin_href = PIN_HREF,
        .pin_pclk = PIN_PCLK,
        // Also clocks the S3's camera peripheral, so it's set even without an XCLK pin
        .xclk_freq_hz = CONFIG_GEMCAM_XCLK_MHZ * 1000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,  // the sensor compresses, so less data crosses the jumpers
        .frame_size = FRAMESIZE_VGA,
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed (%s); checking the wiring...", esp_err_to_name(err));
        diagnose_sccb();
        ESP_LOGE(TAG, "%s", s_error);
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    ESP_LOGI(TAG, "sensor PID 0x%02x", s->id.PID);
    if (s->id.PID != OV2640_PID) {
        ESP_LOGW(TAG, "expected an OV2640 (0x%02x); continuing anyway", OV2640_PID);
    }
    read_status(s);
    load_saved(s);
#if CONFIG_GEMCAM_SENSOR_CLK_DIV > 0
    // 0x111 = sensor bank, CLKRC; the low 6 bits divide the sensor clock
    s->set_reg(s, 0x111, 0x3f, CONFIG_GEMCAM_SENSOR_CLK_DIV);
    ESP_LOGI(TAG, "sensor clock divided by %d", CONFIG_GEMCAM_SENSOR_CLK_DIV + 1);
#endif

    // The sensor answering on SCCB only proves 4 wires; a frame proves the rest
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        snprintf(s_error, sizeof(s_error),
                 "Sensor found but no frames arrive. Check VSYNC->GPIO6, HREF->GPIO7, "
                 "PCLK->GPIO13 and D0-D7.");
        ESP_LOGE(TAG, "%s", s_error);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "first frame: %ux%u, %u bytes", fb->width, fb->height, (unsigned)fb->len);
    esp_camera_fb_return(fb);

    s_ok = true;
    s_error[0] = '\0';
    return ESP_OK;
}

bool gem_camera_ok(void) { return s_ok; }
const char *gem_camera_error(void) { return s_error; }

void gem_camera_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void gem_camera_unlock(void) { xSemaphoreGive(s_lock); }

esp_err_t gem_camera_set(const char *name, int value)
{
    if (!s_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < CTL_COUNT; i++) {
        if (strcmp(name, CONTROLS[i].name) != 0) {
            continue;
        }
        if (value < CONTROLS[i].min || value > CONTROLS[i].max) {
            return ESP_ERR_INVALID_ARG;
        }
        gem_camera_lock();
        int rc = apply(esp_camera_sensor_get(), i, value);
        gem_camera_unlock();
        if (rc != 0) {
            return ESP_FAIL;
        }
        s_values[i] = value;

        nvs_handle_t h;
        if (nvs_open_from_partition(SETTINGS_PART, SETTINGS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i16(h, name, (int16_t)value);
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGI(TAG, "%s = %d", name, value);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

int gem_camera_settings_json(char *buf, size_t len)
{
    int n = snprintf(buf, len, "{");
    for (int i = 0; i < CTL_COUNT && n < (int)len; i++) {
        n += snprintf(buf + n, len - n, "%s\"%s\":%d", i ? "," : "", CONTROLS[i].name, s_values[i]);
    }
    if (n < (int)len) {
        n += snprintf(buf + n, len - n, "}");
    }
    return n;
}
