idf_component_register(SRCS "main.c" "servo.c" "http_server.c"
                    INCLUDE_DIRS "include"
                    REQUIRES driver nvs_flash esp_http_server cJSON esp_wifi
                    EMBED_FILES "../frontend/index.html" "../frontend/script.js")
