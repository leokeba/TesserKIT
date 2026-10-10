// Plays a phone joining a fresh TesserKIT device's access point (DESIGN.md
// §5.2): the captive portal's DNS answers every name with the device, the
// connectivity probe a phone sends is redirected to the interface, which is
// served; then a Wi-Fi network is saved through the API, as the interface
// does. Prints "PROBE PASS <name>" / "PROBE FAIL <name>: ..." and
// "PROBE DONE pass=<n> fail=<n>".
//
//   idf.py -C test/captive_probe -B <dir> -DAP_SSID=<the device's hostname> build flash
//
// A gitignored main/secrets.h defines WIFI_SSID and WIFI_PASSWORD: the
// network to provision.
#include <stdio.h>
#include <string.h>

#include <string>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/netdb.h"
#include "nvs_flash.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef AP_SSID
#define AP_SSID "tesserkit-demo-a67c"
#endif

namespace {

int g_pass = 0, g_fail = 0;
EventGroupHandle_t g_events;
constexpr EventBits_t kGotIp = 1, kLost = 2;

void result(const char* name, bool ok, const std::string& detail = "") {
    (ok ? g_pass : g_fail)++;
    if (ok) {
        printf("PROBE PASS %s\n", name);
    } else {
        printf("PROBE FAIL %s: %s\n", name, detail.c_str());
    }
}

void onEvent(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(g_events, kGotIp);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) xEventGroupSetBits(g_events, kLost);
}

struct Response {
    int status = -1;
    std::string location;
    std::string body;
};

esp_err_t onHttp(esp_http_client_event_t* e) {
    Response* r = static_cast<Response*>(e->user_data);
    if (e->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(e->header_key, "Location") == 0) r->location = e->header_value;
    if (e->event_id == HTTP_EVENT_ON_DATA && r->body.size() < 4096) r->body.append(static_cast<char*>(e->data), e->data_len);
    return ESP_OK;
}

Response http(const char* url, esp_http_client_method_t method = HTTP_METHOD_GET, const char* body = nullptr) {
    Response r;
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 15000;
    config.disable_auto_redirect = true;
    config.event_handler = onHttp;
    config.user_data = &r;
    config.method = method;
    esp_http_client_handle_t c = esp_http_client_init(&config);
    if (body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, body, static_cast<int>(strlen(body)));
    }
    if (esp_http_client_perform(c) == ESP_OK) r.status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    return r;
}

std::string resolve(const char* name) {
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (getaddrinfo(name, nullptr, &hints, &res) != 0 || !res) return "";
    char ip[16];
    inet_ntoa_r(reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr, ip, sizeof(ip));
    freeaddrinfo(res);
    return ip;
}

}  // namespace

extern "C" void app_main() {
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    g_events = xEventGroupCreate();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onEvent, nullptr, nullptr);
    wifi_config_t config = {};
    strncpy(reinterpret_cast<char*>(config.sta.ssid), AP_SSID, sizeof(config.sta.ssid));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &config);
    esp_wifi_start();
    printf("PROBE joining \"%s\"\n", AP_SSID);
    bool joined = false;
    for (int i = 0; i < 10 && !joined; i++) {
        esp_wifi_connect();
        joined = xEventGroupWaitBits(g_events, kGotIp, pdTRUE, pdFALSE, pdMS_TO_TICKS(8000)) & kGotIp;
    }
    result("join_access_point", joined);
    if (!joined) {
        printf("PROBE DONE pass=%d fail=%d\n", g_pass, g_fail);
        return;
    }
    xEventGroupClearBits(g_events, kLost);

    std::string ip = resolve("connectivitycheck.gstatic.com");
    result("dns_any_name", ip == "192.168.4.1", ip);
    result("dns_another_name", resolve("captive.apple.com") == "192.168.4.1");

    Response probe = http("http://connectivitycheck.gstatic.com/generate_204");
    result("probe_redirected", probe.status == 302 && probe.location == "http://192.168.4.1/",
           std::to_string(probe.status) + " " + probe.location);
    Response apple = http("http://captive.apple.com/hotspot-detect.html");
    result("apple_probe_redirected", apple.status == 302, std::to_string(apple.status));
    Response page = http("http://192.168.4.1/");
    result("interface_served", page.status == 200 && page.body.find("TESSER") != std::string::npos,
           std::to_string(page.status));
    Response state = http("http://192.168.4.1/api/sys/status/state");
    result("provisioning", state.body == "\"provisioning\"", state.body);

#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
    std::string networks = std::string("[{\"ssid\":\"") + WIFI_SSID + "\",\"password\":\"" + WIFI_PASSWORD + "\"}]";
    Response saved = http("http://192.168.4.1/api/sys/network/wifi/networks", HTTP_METHOD_PUT, networks.c_str());
    result("network_saved", saved.status == 200, std::to_string(saved.status) + " " + saved.body);
    // The device joins it; the access point stays up for 30 s for phones on
    // it to follow (DESIGN.md §5.2), unless the router's channel moves it.
    int64_t start = esp_timer_get_time();
    std::string joinedAs;
    while (esp_timer_get_time() - start < 25000000 && joinedAs.empty()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        Response st = http("http://192.168.4.1/api/sys/network/wifi/status/ip");
        if (st.status == 200 && st.body.size() > 2) joinedAs = st.body;
    }
    result("device_joined", !joinedAs.empty(), "no address");
    if (!joinedAs.empty()) printf("PROBE device joined as %s\n", joinedAs.c_str());
    Response portal = http("http://connectivitycheck.gstatic.com/generate_204");
    result("portal_off_once_joined", portal.status == 404 || portal.status == -1, std::to_string(portal.status));
    EventBits_t lost = xEventGroupWaitBits(g_events, kLost, pdFALSE, pdFALSE, pdMS_TO_TICKS(60000));
    printf("PROBE access point gone %s after %lld s\n", (lost & kLost) ? "" : "(not)",
           (esp_timer_get_time() - start) / 1000000);
#endif
    printf("PROBE DONE pass=%d fail=%d\n", g_pass, g_fail);
}
