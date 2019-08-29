set(COMPONENT_ADD_INCLUDEDIRS .)

set(COMPONENT_SRCS "dns_server.c" "http_server.c" "json.c" "wifi_manager.c")
set(REQUIRES esp_common)
set(COMPONENT_EMBED_FILES "style.css jquery.gz code.js index.html")

set(REQUIRES_COMPONENTS freertos )

register_component()


