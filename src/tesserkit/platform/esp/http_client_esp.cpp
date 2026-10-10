// Firmware downloads over HTTP and HTTPS (ESP-IDF's certificate bundle).
#if defined(ESP_PLATFORM)

#include "tesserkit/platform.h"

#include <memory>

#include "esp_http_client.h"
#include "sdkconfig.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

namespace tesserkit {

using tesser::Check;
using tesser::Status;

namespace {

constexpr size_t kChunk = 2048;
constexpr int kMaxRedirects = 5;

class EspHttpClient : public HttpClient {
public:
    Check get(const char* url, const std::function<Check(int64_t)>& length,
              const std::function<Check(const uint8_t*, size_t)>& data) override {
        esp_http_client_config_t config = {};
        config.url = url;
        config.timeout_ms = 15000;
        config.buffer_size = 1536;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) return Check::fail(Status::Internal, "no memory for the download");
        Check c = run(client, length, data);
        esp_http_client_cleanup(client);
        return c;
    }

private:
    Check run(esp_http_client_handle_t client, const std::function<Check(int64_t)>& length,
              const std::function<Check(const uint8_t*, size_t)>& data) {
        // Release assets redirect to a CDN: follow, as esp_https_ota does.
        int status = 0;
        for (int i = 0;; i++) {
            if (esp_http_client_open(client, 0) != ESP_OK) return Check::fail(Status::NotFound, "server unreachable");
            if (esp_http_client_fetch_headers(client) < 0) return Check::fail(Status::Timeout, "no answer from the server");
            status = esp_http_client_get_status_code(client);
            bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
            if (!redirect) break;
            if (i == kMaxRedirects) return Check::fail(Status::NotFound, "too many redirects");
            esp_http_client_flush_response(client, nullptr);
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
        }
        if (status == 404) return Check::fail(Status::NotFound, "HTTP 404");
        if (status != 200) return Check::fail(Status::NotFound, "the server refused the download");
        int64_t len = esp_http_client_get_content_length(client);
        Check c = length(len > 0 ? len : -1);
        if (!c.isOk()) return c;
        std::unique_ptr<uint8_t[]> buf(new uint8_t[kChunk]);
        for (;;) {
            int n = esp_http_client_read(client, reinterpret_cast<char*>(buf.get()), kChunk);
            if (n < 0) return Check::fail(Status::Timeout, "download interrupted");
            if (n == 0) break;
            c = data(buf.get(), static_cast<size_t>(n));
            if (!c.isOk()) return c;
        }
        if (!esp_http_client_is_complete_data_received(client)) return Check::fail(Status::Timeout, "download cut short");
        return Check::ok();
    }
};

}  // namespace

HttpClient* espHttpClient() {
    static EspHttpClient client;
    return &client;
}

}  // namespace tesserkit

#endif
