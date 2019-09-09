idf_component_register(SRCS "dns_server.c" "http_server.c" "json.c" "wifi_manager.c"
                       INCLUDE_DIRS .
                       REQUIRES esp_common 
                       PRIV_REQUIRES newlib freertos  spi_flash nvs_flash mdns pthread wpa_supplicant cmd_system
                       EMBED_FILES style.css jquery.gz code.js index.html
)