#include "demo_stubs/demo_runtime.c"
#include "../main/demo_wifi.c"

static unsigned step, fail_at, destroys;
static bool netif_live, driver_registered, defaults_live, wifi_live, wifi_running, handler_live;
static esp_netif_t netif;

static esp_err_t next_step(void) { return ++step == fail_at ? ESP_ERR_NO_MEM : ESP_OK; }

esp_netif_t *esp_netif_new(const esp_netif_config_t *cfg) {
    (void)cfg;
    assert(!netif_live);
    if (next_step() != ESP_OK) return NULL;
    netif_live = true;
    return &netif;
}
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *created) {
    assert(created == &netif && netif_live);
    driver_registered = true; // IDF stores the pointer even when attach fails.
    return next_step();
}
esp_err_t esp_wifi_set_default_wifi_sta_handlers(void) {
    assert(driver_registered);
    esp_err_t result = next_step();
    defaults_live = result == ESP_OK; // IDF rolls back partially added handlers.
    return result;
}
void esp_netif_destroy_default_wifi(void *created) {
    assert(created == &netif && netif_live && !wifi_live && !handler_live);
    netif_live = driver_registered = defaults_live = false;
    destroys++;
}
esp_err_t esp_wifi_init(const wifi_init_config_t *cfg) {
    (void)cfg;
    assert(defaults_live);
    esp_err_t result = next_step();
    wifi_live = result == ESP_OK;
    return result;
}
esp_err_t esp_wifi_deinit(void) {
    assert(wifi_live && !wifi_running && !handler_live);
    wifi_live = false;
    return ESP_OK;
}
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
    void (*callback)(void *, esp_event_base_t, int32_t, void *), void *arg,
    esp_event_handler_instance_t *instance) {
    (void)base; (void)id; (void)callback; (void)arg;
    esp_err_t result = next_step();
    if (result == ESP_OK) { handler_live = true; *instance = &netif; }
    return result;
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
    esp_event_handler_instance_t instance) {
    (void)base; (void)id;
    assert(handler_live && instance == &netif);
    handler_live = false;
    return ESP_OK;
}
esp_err_t esp_wifi_set_storage(int storage) { (void)storage; return next_step(); }
esp_err_t esp_wifi_set_mode(int mode) { (void)mode; return next_step(); }
esp_err_t esp_wifi_start(void) {
    esp_err_t result = next_step();
    wifi_running = result == ESP_OK;
    return result;
}
esp_err_t esp_wifi_stop(void) { assert(wifi_running); wifi_running = false; return ESP_OK; }
esp_err_t esp_wifi_scan_start(const void *cfg, bool blocking) {
    (void)cfg; (void)blocking; assert(wifi_running); return next_step();
}
esp_err_t esp_wifi_scan_stop(void) { assert(wifi_running); return ESP_OK; }

static void assert_clean(void) {
    assert(!s_sta_netif && !s_wifi_initialized && !s_wifi_started && !s_handler_registered);
    assert(!netif_live && !driver_registered && !defaults_live);
    assert(!wifi_live && !wifi_running && !handler_live);
}

int main(void) {
    for (unsigned failure = 1; failure <= 9; failure++) {
        step = 0;
        fail_at = failure;
        unsigned before = destroys;
        assert(demo_wifi_start() == ESP_ERR_NO_MEM);
        assert(step == failure && s_state == WIFI_DEMO_FAILED);
        assert(destroys == before + (failure > 1));
        assert_clean();
        assert(demo_wifi_stop() == ESP_OK);
        assert_clean();
        // Once resources are available, entry after the failure must work.
        step = fail_at = 0;
        assert(demo_wifi_start() == ESP_OK);
        assert(step == 9 && s_state == WIFI_DEMO_SCANNING);
        assert(demo_wifi_start() == ESP_ERR_INVALID_STATE);
        assert(demo_wifi_stop() == ESP_OK);
        assert_clean();
    }
    puts("Wi-Fi allocation/attach/handler/start failure rollback tests: PASS");
    return 0;
}
