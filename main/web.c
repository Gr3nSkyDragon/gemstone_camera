#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gemcam.h"

static const char *TAG = "web";

#define JSON_BUF 8192
#define MAX_UPLOAD (512 * 1024)

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

// Decodes %XX and '+' in place
static void url_decode(char *s)
{
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && r[1] && r[2]) {
            const char hex[3] = {r[1], r[2], 0};
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static bool query_param(httpd_req_t *req, const char *key, char *out, size_t len)
{
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, key, out, len) != ESP_OK) {
        return false;
    }
    url_decode(out);
    return true;
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    char buf[320];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, status);
    return send_json(req, buf);
}

// {"mode":"live","trained":{"samples":n,"labels":[...]},"live":{...}}
static int banks_json(char *buf, size_t len)
{
    int n = snprintf(buf, len, "\"mode\":\"%s\"", gem_bank_name(gem_mode()));
    for (int b = 0; b < GEM_BANK_COUNT && n < (int)len; b++) {
        n += snprintf(buf + n, len - n, ",\"%s\":{\"samples\":%d,\"labels\":",
                      gem_bank_name(b), gem_sample_count(b));
        n += gem_labels_json(b, buf + n, len - n);
        n += snprintf(buf + n, len - n, "}");
    }
    return n;
}

static esp_err_t send_banks(httpd_req_t *req)
{
    char *buf = malloc(JSON_BUF);
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    int n = snprintf(buf, JSON_BUF, "{");
    n += banks_json(buf + n, JSON_BUF - n);
    snprintf(buf + n, JSON_BUF - n, "}");
    esp_err_t err = send_json(req, buf);
    free(buf);
    return err;
}

static esp_err_t send_result(httpd_req_t *req, const gem_result_t *r)
{
    char *buf = malloc(2048);
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    gem_result_json(r, buf, 2048);
    esp_err_t err = send_json(req, buf);
    free(buf);
    return err;
}

// ---- page and camera ------------------------------------------------------------

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    // EMBED_TXTFILES adds a trailing NUL
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t capture_get(httpd_req_t *req)
{
    if (!gem_camera_ok()) {
        return send_error(req, "503 Service Unavailable", gem_camera_error());
    }
    gem_camera_lock();
    camera_fb_t *fb = gem_camera_grab();
    if (!fb) {
        gem_camera_unlock();
        return send_error(req, "503 Service Unavailable", gem_camera_error()[0] ? gem_camera_error()
                                                                             : "No frame from the camera");
    }
    // Lets the page notice results from the BOOT button without extra requests
    char seq[12];
    snprintf(seq, sizeof(seq), "%u", gem_result_seq());
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Result-Seq", seq);
    esp_err_t err = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    gem_camera_unlock();
    return err;
}

static esp_err_t status_get(httpd_req_t *req)
{
    char *buf = malloc(JSON_BUF);
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    int n = snprintf(buf, JSON_BUF,
                     "{\"camera_ok\":%s,\"error\":\"%s\",\"uptime_s\":%lld,"
                     "\"free_heap\":%u,\"free_psram\":%u,\"seq\":%u,",
                     gem_camera_ok() ? "true" : "false", gem_camera_error(),
                     esp_timer_get_time() / 1000000,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     gem_result_seq());
    n += banks_json(buf + n, JSON_BUF - n);
    n += snprintf(buf + n, JSON_BUF - n, ",\"settings\":");
    n += gem_camera_settings_json(buf + n, JSON_BUF - n);
    snprintf(buf + n, JSON_BUF - n, "}");
    esp_err_t err = send_json(req, buf);
    free(buf);
    return err;
}

static esp_err_t control_get(httpd_req_t *req)
{
    char var[32], val[16];
    if (!query_param(req, "var", var, sizeof(var)) || !query_param(req, "val", val, sizeof(val))) {
        return send_error(req, "400 Bad Request", "Need var and val");
    }
    esp_err_t err = gem_camera_set(var, atoi(val));
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", esp_err_to_name(err));
    }
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t camera_restart_post(httpd_req_t *req)
{
    gem_camera_restart();
    char buf[320];
    snprintf(buf, sizeof(buf), "{\"camera_ok\":%s,\"error\":\"%s\"}",
             gem_camera_ok() ? "true" : "false", gem_camera_error());
    return send_json(req, buf);
}

// ---- identify -------------------------------------------------------------------

static esp_err_t identify_post(httpd_req_t *req)
{
    gem_result_t *r = malloc(sizeof(*r));
    if (!r) {
        return httpd_resp_send_500(req);
    }
    esp_err_t err = gem_identify_now(r);
    esp_err_t sent;
    if (err == ESP_OK) {
        sent = send_result(req, r);
    } else if (err == ESP_ERR_NOT_FOUND) {
        sent = send_error(req, "409 Conflict", gem_mode() == GEM_BANK_LIVE
                          ? "Nothing taught yet - teach at least one stone first"
                          : "No trained samples yet - run tools/train.py, or switch to Live demo");
    } else {
        sent = send_error(req, "500 Internal Server Error",
                          gem_camera_ok() ? esp_err_to_name(err) : gem_camera_error());
    }
    free(r);
    return sent;
}

static esp_err_t result_get(httpd_req_t *req)
{
    gem_result_t *r = malloc(sizeof(*r));
    if (!r) {
        return httpd_resp_send_500(req);
    }
    esp_err_t sent = gem_last_result(r) ? send_result(req, r)
                                        : send_error(req, "404 Not Found", "No result yet");
    free(r);
    return sent;
}

static esp_err_t mode_post(httpd_req_t *req)
{
    char name[16];
    gem_bank_t b;
    if (!query_param(req, "set", name, sizeof(name)) || !gem_bank_from_name(name, &b)) {
        return send_error(req, "400 Bad Request", "set=trained or set=live");
    }
    gem_set_mode(b);
    return send_banks(req);
}

// ---- live bank --------------------------------------------------------------------

static esp_err_t teach_post(httpd_req_t *req)
{
    char label[64];
    if (!query_param(req, "label", label, sizeof(label)) || !gem_sanitize_label(label)) {
        return send_error(req, "400 Bad Request", "Give the stone a name (letters, numbers, spaces)");
    }
    gem_features_t f;
    esp_err_t err = gem_capture_features(3, &f);
    if (err == ESP_OK) {
        err = gem_teach(label, &f);
    }
    if (err == ESP_ERR_NO_MEM) {
        return send_error(req, "507 Insufficient Storage", "Live demo memory is full - wipe some stones");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error",
                          gem_camera_ok() ? esp_err_to_name(err) : gem_camera_error());
    }
    return send_banks(req);
}

static esp_err_t forget_post(httpd_req_t *req)
{
    char label[64];
    char all[4];
    if (query_param(req, "all", all, sizeof(all))) {
        gem_forget(NULL);
    } else if (query_param(req, "label", label, sizeof(label)) && gem_sanitize_label(label)) {
        gem_forget(label);
    } else {
        return send_error(req, "400 Bad Request", "Say which label to forget, or all=1");
    }
    return send_banks(req);
}

// ---- trained bank (tools/train.py) --------------------------------------------------

static esp_err_t train_clear_post(httpd_req_t *req)
{
    gem_train_clear();
    return send_banks(req);
}

static esp_err_t train_add_post(httpd_req_t *req)
{
    char label[64];
    if (!query_param(req, "label", label, sizeof(label)) || !gem_sanitize_label(label)) {
        return send_error(req, "400 Bad Request", "Need a label");
    }
    if (req->content_len == 0 || req->content_len > MAX_UPLOAD) {
        return send_error(req, "413 Payload Too Large", "Send one JPEG under 512 KB");
    }
    uint8_t *jpg = heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM);
    if (!jpg) {
        return httpd_resp_send_500(req);
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, (char *)jpg + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            heap_caps_free(jpg);
            return ESP_FAIL;  // connection dropped; httpd closes it
        }
        got += r;
    }
    esp_err_t err = gem_train_add_jpeg(label, jpg, got);
    heap_caps_free(jpg);
    if (err == ESP_ERR_NO_MEM) {
        return send_error(req, "507 Insufficient Storage", "Trained memory is full (1000 photos)");
    }
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", "Couldn't read that JPEG");
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"samples\":%d}", gem_sample_count(GEM_BANK_TRAINED));
    return send_json(req, buf);
}

static esp_err_t train_commit_post(httpd_req_t *req)
{
    if (gem_train_commit() != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "Saving to flash failed");
    }
    return send_banks(req);
}

esp_err_t gem_web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;

    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t uris[] = {
        {.uri = "/",               .method = HTTP_GET,  .handler = index_get},
        {.uri = "/capture",        .method = HTTP_GET,  .handler = capture_get},
        {.uri = "/status",         .method = HTTP_GET,  .handler = status_get},
        {.uri = "/control",        .method = HTTP_GET,  .handler = control_get},
        {.uri = "/result",         .method = HTTP_GET,  .handler = result_get},
        {.uri = "/identify",       .method = HTTP_POST, .handler = identify_post},
        {.uri = "/mode",           .method = HTTP_POST, .handler = mode_post},
        {.uri = "/teach",          .method = HTTP_POST, .handler = teach_post},
        {.uri = "/forget",         .method = HTTP_POST, .handler = forget_post},
        {.uri = "/train/clear",    .method = HTTP_POST, .handler = train_clear_post},
        {.uri = "/train/add",      .method = HTTP_POST, .handler = train_add_post},
        {.uri = "/train/commit",   .method = HTTP_POST, .handler = train_commit_post},
        {.uri = "/camera/restart", .method = HTTP_POST, .handler = camera_restart_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    ESP_LOGI(TAG, "web server started");
    return ESP_OK;
}
