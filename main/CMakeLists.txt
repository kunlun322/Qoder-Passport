# 「Qoder Passport」应用。BSP 参考 demo（demo_*.c / ui_pixel*.c / demo_navigation.c）
# 保留在仓库中供查阅与 host 测试，但不再编入固件。
idf_component_register(
    SRCS "main.c"
         "app_ui.c"
         "app_fonts.c"
         "app_wifi.c"
         "app_portal.c"
         "portal_form.c"
         "qca_pat.c"
         "qca_api.c"
         "qca_list_parse.c"
         "qca_voice.c"
         "tls_mfl.c"
         "qjson.c"
         "b64_stream.c"
         "audio_dsp.c"
         "voice_logic.c"
         "utf8_util.c"
         "../assets/fonts/app_font_cjk_16.c"
         "../assets/fonts/app_font_status_28.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             esp_http_client esp_http_server lwip esp-tls mbedtls esp_system
    EMBED_TXTFILES "portal.html"
)

# 见 tls_mfl.c：包装 esp-tls 调用的 mbedtls_ssl_config_defaults，为 TLS 客户端请求 4KB
# 最大分片，避免语音会话中为满长记录申请约 16.7KB 连续接收缓冲。-u 确保包装函数被链接进来。
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=mbedtls_ssl_config_defaults"
    "-Wl,-u,__wrap_mbedtls_ssl_config_defaults")
