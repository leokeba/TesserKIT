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
#include "esp_partition.h"
#include "nvs_flash.h"

namespace tesserkit {

namespace {

// Set by the device task, read by the HTTP server's: the address as one
// atomic word, formatted by the reader, so a redirect never reads a URL
// being rewritten.
std::atomic<bool> g_captive{false};
std::atomic<uint32_t> g_portalIp{0xC0A80401};  // 192.168.4.1

// Unknown URIs: while the captive portal is on, every one leads to the
// interface, so the probes phones and laptops send on joining open it.
esp_err_t notFound(httpd_req_t* req, httpd_err_code_t) {
    if (g_captive.load()) {
        uint32_t ip = g_portalIp.load();
        char url[24];  // "http://255.255.255.255/"
        snprintf(url, sizeof(url), "http://%u.%u.%u.%u/", static_cast<unsigned>(ip >> 24),
                 static_cast<unsigned>(ip >> 16 & 255), static_cast<unsigned>(ip >> 8 & 255),
                 static_cast<unsigned>(ip & 255));
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", url);  // kept until the send below
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, nullptr, 0);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
}

void deviceTask(void* arg) {
    Device* dev = static_cast<Device*>(arg);
    const TickType_t period = pdMS_TO_TICKS(20);
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        dev->loop();
        next += period;
        // Until the next period, the pollers run whenever a transport wakes
        // the task (Device::wake()).
        for (;;) {
            TickType_t now = xTaskGetTickCount();
            if (static_cast<int32_t>(next - now) <= 0) {
                if (static_cast<int32_t>(now - next) > static_cast<int32_t>(period)) next = now;  // late: don't catch up
                break;
            }
            if (ulTaskNotifyTake(pdTRUE, next - now)) dev->poll();
        }
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
    g_portalIp.store(apIp);
    g_captive.store(on);
}
}  // namespace esp

bool Device::startServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = httpPort_;
    config.stack_size = 6144;  // measured: 3 KB used serving the interface, uploads and schemas
    config.lru_purge_enable = true;
    // What is used, never set by hand: the API (4, CORS 1, WebSocket 1), the
    // page, the bundle and its widgets' chunks (3), and the application's
    // (assets included).
    config.max_uri_handlers = static_cast<uint16_t>(4 + (cors_ ? 1 : 0) + 1 + (ui_ ? 3 : 0) + extraHandlers_);
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

void Device::wake() {
    void* task = task_.load();
    if (task) xTaskNotifyGive(static_cast<TaskHandle_t>(task));
}

bool Device::startTask() {
    // Measured: the services use 1.3 KB. A transport polled here also runs
    // its requests here, which take up to 5 KB as in the HTTP task.
    uint32_t stack = pollers_.empty() ? 6144 : 8192;
    // Their work runs at the priority their own tasks had (NowTP's and the
    // HTTP server's, 5): at 3, a relay through a gateway waits behind the
    // HTTP task that feeds it (DESIGN.md section 4.2).
    UBaseType_t priority = pollers_.empty() ? 3 : 5;
    TaskHandle_t task = nullptr;
    if (xTaskCreate(deviceTask, "tesserkit", stack, this, priority, &task) != pdPASS) {
        tesser::logError("tesserkit: no memory for the task");
        return false;
    }
    task_ = task;
    return true;
}

tesser::Storage* Device::nvsStorage() {
    if (platform_.nvs) return platform_.nvs;
    // Arduino initializes NVS at boot; ESP-IDF applications may not have.
    // Readable settings are never erased here: that is a factory reset's
    // decision.
    esp_err_t err = esp::nvsInit();
    if (err != ESP_OK) {
        tesser::logError("tesserkit: NVS unavailable (%s)", esp_err_to_name(err));
        return nullptr;
    }
    // Only TesserAPI's records live in this namespace: one per top-level
    // node, and "@version".
    static tesser::NvsStorage storage("tesserkit");
    if (!capacitySet_) {
        const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
        settingsCapacity_ = p ? static_cast<uint32_t>(p->size) : 0;
    }
    platform_.nvs = &storage;
    return &storage;
}

}  // namespace tesserkit

#endif
