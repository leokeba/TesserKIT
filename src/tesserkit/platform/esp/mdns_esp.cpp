// <hostname>.local and the _http._tcp service, on ESP-IDF's mdns component.
// Left out with TESSERKIT_NO_MDNS.
#if defined(ESP_PLATFORM) && !defined(TESSERKIT_NO_MDNS)

#include "tesser/platform.h"
#include "tesserkit/platform.h"

#include <string>

#include "mdns.h"

namespace tesserkit {

namespace {

class EspMdns : public Mdns {
public:
    void set(bool on, const char* hostname, const char* version, uint16_t port) override {
        if (!on) {
            if (up_) mdns_free();
            up_ = false;
            return;
        }
        if (!up_) {
            esp_err_t err = mdns_init();
            if (err != ESP_OK) {
                tesser::logError("tesserkit: mDNS: %s", esp_err_to_name(err));
                return;
            }
            up_ = true;
        }
        mdns_hostname_set(hostname);
        mdns_instance_name_set(hostname);
        if (mdns_service_exists("_http", "_tcp", nullptr)) mdns_service_remove("_http", "_tcp");
        if (!port) return;
        version_ = version;
        mdns_txt_item_t txt[] = {
            {"api", "/api"},
            {"ws", "/ws"},
            {"version", version_.c_str()},
            {"tesser", "1"},
        };
        mdns_service_add(nullptr, "_http", "_tcp", port, txt, sizeof(txt) / sizeof(txt[0]));
    }

private:
    std::string version_;
    bool up_ = false;
};

}  // namespace

Mdns* espMdns() {
    static EspMdns mdns;
    return &mdns;
}

}  // namespace tesserkit

#endif
