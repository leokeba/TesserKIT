// Firmware downloads and release checks over HTTP and HTTPS (ESP-IDF's
// certificate bundle).
#if defined(ESP_PLATFORM)

#include "tesserkit/platform.h"

#include "esp_http_client.h"
#include "sdkconfig.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

namespace tesserkit {

using tesser::Check;
using tesser::Status;

namespace {

constexpr int kMaxRedirects = 5;

class EspResponse : public HttpResponse {
public:
    EspResponse(esp_http_client_handle_t client, int status) : client_(client), status_(status) {}
    int status() override { return status_; }
    int64_t length() override {
        int64_t len = esp_http_client_get_content_length(client_);
        return len > 0 ? len : -1;
    }
    int read(uint8_t* buf, size_t len) override {
        int n = esp_http_client_read(client_, reinterpret_cast<char*>(buf), static_cast<int>(len));
        if (n < 0) return -1;
        if (n == 0 && !esp_http_client_is_complete_data_received(client_)) return -1;
        return n;
    }

private:
    esp_http_client_handle_t client_;
    int status_;
};

class EspHttpClient : public HttpClient {
public:
    Check get(const char* url, const std::function<Check(HttpResponse&)>& fn) override {
        esp_http_client_config_t config = {};
        config.url = url;
        config.timeout_ms = 15000;
        config.buffer_size = 1536;
        // The request line holds the whole URL: GitHub's assets redirect to a
        // signed CDN URL of about 950 characters, which the default 512 bytes
        // refused ("Out of buffer").
        config.buffer_size_tx = 2048;
        config.user_agent = "TesserKIT";  // GitHub's API requires one
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) return Check::fail(Status::Internal, "no memory for the request");
        Check c = run(client, fn);
        esp_http_client_cleanup(client);
        return c;
    }

private:
    Check run(esp_http_client_handle_t client, const std::function<Check(HttpResponse&)>& fn) {
        // Release assets redirect to a CDN: follow, as esp_https_ota does.
        int status = 0;
        for (int i = 0;; i++) {
            esp_err_t err = esp_http_client_open(client, 0);
            if (err == ESP_ERR_HTTP_CONNECT) return Check::fail(Status::NotFound, "server unreachable");
            if (err != ESP_OK) return Check::fail(Status::Internal, "the request couldn't be sent");
            if (esp_http_client_fetch_headers(client) < 0) return Check::fail(Status::Timeout, "no answer from the server");
            status = esp_http_client_get_status_code(client);
            bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
            if (!redirect) break;
            if (i == kMaxRedirects) return Check::fail(Status::NotFound, "too many redirects");
            esp_http_client_flush_response(client, nullptr);
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
        }
        EspResponse response(client, status);
        return fn(response);
    }
};

}  // namespace

HttpClient* espHttpClient() {
    static EspHttpClient client;
    return &client;
}

}  // namespace tesserkit

#endif
