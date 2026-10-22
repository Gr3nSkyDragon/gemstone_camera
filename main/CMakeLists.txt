idf_component_register(SRCS "main.c" "camera.c" "gem.c" "web.c"
                    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server driver esp_timer
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES "index.html")
