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

// "mode":"live","trained":{"name":...,"labels":[...]},"live":{"samples":n,"labels":[...]}
static int modes_json(char *buf, size_t len)
{
    int n = snprintf(buf, len, "\"mode\":\"%s\",\"trained\":", gem_mode_name(gem_mode()));
    n += gem_ai_info_json(buf + n, len - n);
    n += snprintf(buf + n, len - n, ",\"live\":{\"samples\":%d,\"labels\":", gem_live_count());
    n += gem_live_labels_json(buf + n, len - n);
    n += snprintf(buf + n, len - n, "}");
    return n;
}

static esp_err_t send_modes(httpd_req_t *req)
{
    char *buf = malloc(JSON_BUF);
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    int n = snprintf(buf, JSON_BUF, "{");
    n += modes_json(buf + n, JSON_BUF - n);
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
    n += modes_json(buf + n, JSON_BUF - n);
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
        sent = send_error(req, "409 Conflict", "Nothing taught yet - teach at least one stone first");
    } else {
        sent = send_error(req, "500 Internal Server Error",
                          gem_camera_ok() ? esp_err_to_name(err) : gem_camera_error());
    }
    free(r);
    return sent;
}

// ---- debug: classify an uploaded photo ------------------------------------------

#define CLASSIFY_MAX_JPEG (512 * 1024)
#define CLASSIFY_MAX_INPUT (160 * 160 * 3)

static void put_le(uint8_t *p, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

// 24-bit BMP of an RGB888 image: rows bottom-up in B,G,R order, padded to 4 bytes
static esp_err_t send_bmp(httpd_req_t *req, const uint8_t *rgb, int w, int h)
{
    const int row = (w * 3 + 3) & ~3;
    const uint32_t size = 54 + (uint32_t)row * h;
    uint8_t *bmp = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
    if (!bmp) {
        return httpd_resp_send_500(req);
    }
    bmp[0] = 'B';
    bmp[1] = 'M';
    put_le(bmp + 2, size, 4);
    put_le(bmp + 10, 54, 4);   // pixel data offset
    put_le(bmp + 14, 40, 4);   // info header size
    put_le(bmp + 18, w, 4);
    put_le(bmp + 22, h, 4);
    put_le(bmp + 26, 1, 2);    // planes
    put_le(bmp + 28, 24, 2);   // bits per pixel
    for (int y = 0; y < h; y++) {
        const uint8_t *s = rgb + (size_t)(h - 1 - y) * w * 3;
        uint8_t *d = bmp + 54 + (size_t)y * row;
        for (int x = 0; x < w; x++, s += 3, d += 3) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
        }
    }
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, (const char *)bmp, size);
    heap_caps_free(bmp);
    return err;
}

// POST a JPEG (raw body, e.g. a photo from dataset/) to run it through exactly
// the same decode -> crop/resize -> model path as a camera frame.
//   /classify             -> the result as JSON, like /identify
//   /classify?view=input  -> the 96x96 image the model actually saw, as a BMP
static esp_err_t classify_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > CLASSIFY_MAX_JPEG) {
        return send_error(req, "413 Payload Too Large", "Send one JPEG of up to 512 KB as the request body");
    }
    char view[8] = "";
    const bool want_input = query_param(req, "view", view, sizeof(view)) && strcmp(view, "input") == 0;

    uint8_t *jpg = heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM);
    uint8_t *input = want_input ? heap_caps_malloc(CLASSIFY_MAX_INPUT, MALLOC_CAP_SPIRAM) : NULL;
    gem_result_t *r = malloc(sizeof(*r));
    esp_err_t sent;
    if (!jpg || !r || (want_input && !input)) {
        sent = httpd_resp_send_500(req);
        goto done;
    }

    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, (char *)jpg + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (n <= 0) {
            sent = ESP_FAIL;  // connection dropped; nothing to reply to
            goto done;
        }
        got += n;
    }
    if (got < 2 || jpg[0] != 0xFF || jpg[1] != 0xD8) {
        sent = send_error(req, "415 Unsupported Media Type", "That isn't a JPEG");
        goto done;
    }

    int iw = 0, ih = 0;
    esp_err_t err = gem_classify_jpeg(jpg, got, r, input, CLASSIFY_MAX_INPUT, &iw, &ih);
    if (err != ESP_OK) {
        sent = send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    } else if (want_input) {
        sent = send_bmp(req, input, iw, ih);
    } else {
        sent = send_result(req, r);
    }

done:
    heap_caps_free(jpg);
    heap_caps_free(input);
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
    gem_mode_t m;
    if (!query_param(req, "set", name, sizeof(name)) || !gem_mode_from_name(name, &m)) {
        return send_error(req, "400 Bad Request", "set=trained or set=live");
    }
    gem_set_mode(m);
    return send_modes(req);
}

// ---- live demo --------------------------------------------------------------------

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
    return send_modes(req);
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
    return send_modes(req);
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
        {.uri = "/classify",       .method = HTTP_POST, .handler = classify_post},
        {.uri = "/mode",           .method = HTTP_POST, .handler = mode_post},
        {.uri = "/teach",          .method = HTTP_POST, .handler = teach_post},
        {.uri = "/forget",         .method = HTTP_POST, .handler = forget_post},
        {.uri = "/camera/restart", .method = HTTP_POST, .handler = camera_restart_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    ESP_LOGI(TAG, "web server started");
    return ESP_OK;
}
