// The device's ESP side: its HTTP server with the API, WebSocket and the
// interface, the captive portal's redirect, the task, and NVS.
#if defined(ESP_PLATFORM)

#include "tesserkit/device.h"
#include "tesserkit/platform/esp/esp_internal.h"

#include <stdio.h>

#include <atomic>

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

namespace tesserkit {

namespace {

std::atomic<bool> g_captive{false};
char g_portalUrl[32] = "http://192.168.4.1/";

// Unknown URIs: while the captive portal is on, every one leads to the
// interface, so the probes phones and laptops send on joining open it.
esp_err_t notFound(httpd_req_t* req, httpd_err_code_t) {
    if (g_captive.load()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", g_portalUrl);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, nullptr, 0);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
}

void deviceTask(void* arg) {
    Device* dev = static_cast<Device*>(arg);
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        dev->loop();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
    }
}

}  // namespace

namespace esp {
esp_err_t nvsInit() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        tesser::logWarning("tesserkit: NVS unusable (%s), erasing it", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    return err;
}

void captiveRedirect(bool on, uint32_t apIp) {
    snprintf(g_portalUrl, sizeof(g_portalUrl), "http://%u.%u.%u.%u/", static_cast<unsigned>(apIp >> 24),
             static_cast<unsigned>(apIp >> 16 & 255), static_cast<unsigned>(apIp >> 8 & 255),
             static_cast<unsigned>(apIp & 255));
    g_captive.store(on);
}
}  // namespace esp

bool Device::startServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = httpPort_;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    // What is used, never set by hand: the API (4, CORS 1, WebSocket 1), the
    // page and the bundle (2), and the application's (assets included).
    config.max_uri_handlers = static_cast<uint16_t>(4 + (cors_ ? 1 : 0) + 1 + (ui_ ? 2 : 0) + extraHandlers_);
    if (cors_) http_.enableCors(cors_);
    http_.enableWebSocket("/ws");
    esp_err_t err = http_.begin(config, "/api");
    if (err != ESP_OK) {
        tesser::logError("tesserkit: HTTP server failed to start (%s)", esp_err_to_name(err));
        return false;
    }
    if (ui_) {
        err = web_.serve(http_.handle(), "/api", "/ws");
        if (err != ESP_OK) tesser::logError("tesserkit: the interface couldn't be served (%s)", esp_err_to_name(err));
    }
    httpd_register_err_handler(http_.handle(), HTTPD_404_NOT_FOUND, notFound);
    return true;
}

bool Device::startTask() {
    if (xTaskCreate(deviceTask, "tesserkit", 6144, this, 3, nullptr) != pdPASS) {
        tesser::logError("tesserkit: no memory for the task");
        return false;
    }
    return true;
}

tesser::Storage* Device::defaultStorage() {
    // Arduino initializes NVS at boot; ESP-IDF applications may not have.
    // Readable settings are never erased here: that is a factory reset's
    // decision.
    esp_err_t err = esp::nvsInit();
    if (err != ESP_OK) {
        tesser::logError("tesserkit: NVS unavailable (%s): settings won't persist", esp_err_to_name(err));
        return nullptr;
    }
    static tesser::NvsStorage storage("tesserkit");
    return &storage;
}

}  // namespace tesserkit

#endif
