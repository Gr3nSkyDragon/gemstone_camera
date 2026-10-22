// Gemstone Cam: ESP32-S3 N16R8 + OV2640 gemstone identifier (alpha)
//
// Open the web page the serial log points to, teach each stone a few times,
// then press Identify (or the BOOT button) to have the board name it.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "gemcam.h"

static const char *TAG = "main";

#define BUTTON_GPIO GPIO_NUM_0   // BOOT button on the dev board
#define STA_TIMEOUT_MS 15000

static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED BIT0
static bool s_ap_mode;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED);
        if (!s_ap_mode) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "joined \"%s\" -> open http://" IPSTR "/", CONFIG_GEMCAM_WIFI_SSID, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "a device joined the hotspot");
    }
}

static void start_hotspot(void)
{
    wifi_config_t ap = {
        .ap = {
            .channel = 6,
            .max_connection = 4,
            .authmode = strlen(CONFIG_GEMCAM_AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap.ap.ssid, CONFIG_GEMCAM_AP_SSID, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, CONFIG_GEMCAM_AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(CONFIG_GEMCAM_AP_SSID);

    s_ap_mode = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "hotspot \"%s\" (password \"%s\") -> join it, then open http://192.168.4.1/",
             CONFIG_GEMCAM_AP_SSID, CONFIG_GEMCAM_AP_PASSWORD);
}

static void wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    if (strlen(CONFIG_GEMCAM_WIFI_SSID) == 0) {
        start_hotspot();
        return;
    }

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, CONFIG_GEMCAM_WIFI_SSID, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, CONFIG_GEMCAM_WIFI_PASSWORD, sizeof(sta.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "joining \"%s\"...", CONFIG_GEMCAM_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(STA_TIMEOUT_MS));
    if (!(bits & WIFI_CONNECTED)) {
        ESP_LOGW(TAG, "couldn't join \"%s\"; falling back to the hotspot", CONFIG_GEMCAM_WIFI_SSID);
        s_ap_mode = true;
        esp_wifi_stop();
        start_hotspot();
    }
}

// Press BOOT to identify whatever is under the camera, no laptop needed
static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    int last = 1;
    for (;;) {
        const int level = gpio_get_level(BUTTON_GPIO);
        if (last == 1 && level == 0) {
            gem_result_t r;
            esp_err_t err = gem_identify_now(&r);
            if (err == ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "nothing taught yet - use the web page to teach some stones");
            } else if (err != ESP_OK) {
                ESP_LOGE(TAG, "identify failed: %s", esp_err_to_name(err));
            }
        }
        last = level;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Gemstone Cam alpha");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(gem_store_init());

    // Keep going if the camera fails, so the web page can say what's wrong
    if (gem_camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "camera failed - the web page will show the error");
    }

    wifi_start();
    ESP_ERROR_CHECK(gem_web_start());
    xTaskCreate(button_task, "button", 4096, NULL, 5, NULL);

    // Repeat the problem so a serial monitor opened late still sees it
    while (!gem_camera_ok()) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGE(TAG, "camera: %s", gem_camera_error());
    }
}
