// <hostname>.local with the _http._tcp and _tesser._tcp services, on ESP-IDF's
// mdns component.
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
        for (const char* type : {"_http", "_tesser"}) {
            if (mdns_service_exists(type, "_tcp", nullptr)) mdns_service_remove(type, "_tcp");
        }
        if (!port) return;
        version_ = version;
        mdns_txt_item_t txt[] = {
            {"api", "/api"},
            {"ws", "/ws"},
            {"version", version_.c_str()},
            {"tesser", "1"},
        };
        mdns_service_add(nullptr, "_http", "_tcp", port, txt, sizeof(txt) / sizeof(txt[0]));
        // What TesserAPI's HttpServer::advertise() announces, and TesserUI's
        // bridge browses for the launcher (TesserUI's DESIGN.md section 9.7).
        mdns_service_add(nullptr, "_tesser", "_tcp", port, txt, 2);  // api and ws
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
