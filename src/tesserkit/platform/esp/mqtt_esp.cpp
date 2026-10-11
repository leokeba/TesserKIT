// MQTT on ESP-IDF's esp-mqtt (a component of ESP-IDF 5, espressif/mqtt from
// the component registry since ESP-IDF 6). esp-mqtt runs its own task and
// reconnects by itself; its events are posted to the device. Messages go
// through a queue to a publishing task of ours: esp_mqtt_client_publish()
// blocks on the network, and esp-mqtt's own queue (enqueue()) sends one
// message per pass of its task, which waits up to a second for input.
#if defined(ESP_PLATFORM)

#include "tesserkit/platform.h"

#if __has_include("mqtt_client.h")

#include <errno.h>
#include <string.h>

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_tls_errors.h"
#include "mqtt_client.h"
#include "sdkconfig.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

namespace tesserkit {

namespace {

constexpr uint64_t kOutboxLimit = 8 * 1024;  // esp-mqtt's: QoS 1 subscriptions and the will
constexpr size_t kQueueLength = 32;           // messages waiting for the publishing task
constexpr size_t kQueueBytes = 16 * 1024;     // and their bytes

// A session's state, shared with esp-mqtt's task through the event handler.
// Freed once its client is destroyed, when no handler can run any more.
struct Session {
    int32_t id = 0;
    EventSink sink;
    std::string willTopic, willPayload;
    std::vector<std::string> subscriptions;
    std::string error;  // the last error, told with the next disconnection
    std::atomic<bool> live{true};
    std::atomic<bool> connected{false};
    std::atomic<int> queued{0};  // its messages in the queue: it outlives them
    esp_mqtt_client_handle_t handle = nullptr;
};

struct Message {
    Session* session;
    std::string topic, payload;
    bool retain;
};

QueueHandle_t g_queue = nullptr;
std::atomic<size_t> g_queuedBytes{0};

// Publishes what the device's task queued, in order, blocking on the
// network here rather than there.
void publishTask(void*) {
    for (;;) {
        Message* m = nullptr;
        if (xQueueReceive(g_queue, &m, portMAX_DELAY) != pdTRUE || !m) continue;
        Session* s = m->session;
        if (s->live.load() && s->connected.load()) {
            esp_mqtt_client_publish(s->handle, m->topic.c_str(), m->payload.data(), static_cast<int>(m->payload.size()), 0,
                                    m->retain ? 1 : 0);
        }
        g_queuedBytes.fetch_sub(m->topic.size() + m->payload.size());
        s->queued.fetch_sub(1);
        delete m;
    }
}

void post(Session* s, Event::Type type, std::string text = std::string(), std::string data = std::string()) {
    if (!s->live.load()) return;
    Event e;
    e.type = type;
    e.value = s->id;
    e.text = std::move(text);
    e.data = std::move(data);
    s->sink(std::move(e));
}

std::string describe(const esp_mqtt_error_codes_t* e) {
    if (!e) return "error";
    if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        switch (e->connect_return_code) {
            case MQTT_CONNECTION_REFUSE_PROTOCOL: return "refused: protocol version";
            case MQTT_CONNECTION_REFUSE_ID_REJECTED: return "refused: client ID rejected";
            case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE: return "refused: broker unavailable";
            case MQTT_CONNECTION_REFUSE_BAD_USERNAME: return "refused: bad user name or password";
            case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED: return "refused: not authorized";
            default: return "refused by the broker";
        }
    }
    if (e->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        if (e->esp_transport_sock_errno) return std::string("can't reach the broker: ") + strerror(e->esp_transport_sock_errno);
        switch (e->esp_tls_last_esp_err) {
            case ESP_OK: break;
            case ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME: return "can't resolve the broker's name";
            case ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT: return "the broker didn't answer";
            default: return std::string("can't reach the broker: ") + esp_err_to_name(e->esp_tls_last_esp_err);
        }
        return "can't reach the broker";
    }
    if (e->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) return "subscription refused";
    return "error";
}

void onEvent(void* arg, esp_event_base_t, int32_t id, void* data) {
    Session* s = static_cast<Session*>(arg);
    auto* ev = static_cast<esp_mqtt_event_handle_t>(data);
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
        case MQTT_EVENT_CONNECTED:
            s->connected.store(true);
            s->error.clear();
            // A clean session forgets subscriptions: again on each connection.
            for (const std::string& f : s->subscriptions) esp_mqtt_client_subscribe(ev->client, f.c_str(), 1);
            post(s, Event::Type::MqttConnected);
            break;
        case MQTT_EVENT_DISCONNECTED:
            s->connected.store(false);
            post(s, Event::Type::MqttDisconnected, s->error.empty() ? "connection lost" : s->error);
            break;
        case MQTT_EVENT_ERROR:
            s->error = describe(ev->error_handle);
            // A failed connection attempt: say why at once.
            if (!s->connected.load()) post(s, Event::Type::MqttDisconnected, s->error);
            break;
        case MQTT_EVENT_DATA:
            // Messages larger than the client's buffer come in pieces: not
            // commands, ignored.
            if (ev->current_data_offset == 0 && ev->data_len == ev->total_data_len) {
                post(s, Event::Type::MqttMessage, std::string(ev->topic, static_cast<size_t>(ev->topic_len)),
                     std::string(ev->data, static_cast<size_t>(ev->data_len)));
            }
            break;
        default:
            break;
    }
}

// Publishes the will, disconnects and frees the session. Blocking: in a
// task of its own.
void finish(Session* s) {
    // The publishing task skips a stopped session's messages: they go fast.
    while (s->queued.load() > 0) vTaskDelay(pdMS_TO_TICKS(10));
    if (s->connected.load()) {
        esp_mqtt_client_publish(s->handle, s->willTopic.c_str(), s->willPayload.data(),
                                static_cast<int>(s->willPayload.size()), 0, 1);
        esp_mqtt_client_disconnect(s->handle);
    }
    esp_mqtt_client_destroy(s->handle);
    delete s;
}

void finishTask(void* arg) {
    finish(static_cast<Session*>(arg));
    vTaskDelete(nullptr);
}

class EspMqtt : public MqttClient {
public:
    explicit EspMqtt(esp_err_t (*attach)(void*)) : attach_(attach) {}

    int32_t start(const MqttConfig& c, EventSink sink) override {
        stop();
        if (!g_queue) {
            g_queue = xQueueCreate(kQueueLength, sizeof(Message*));
            if (!g_queue) return 0;
            if (xTaskCreate(publishTask, "tk_mqtt_pub", 4096, nullptr, 4, nullptr) != pdPASS) {
                vQueueDelete(g_queue);
                g_queue = nullptr;
                return 0;
            }
        }
        Session* s = new Session();
        s->id = ++sessions_;
        s->sink = std::move(sink);
        s->willTopic = c.willTopic;
        s->willPayload = c.willPayload;
        s->subscriptions = c.subscriptions;
        // esp-mqtt copies the configuration's strings.
        esp_mqtt_client_config_t cfg = {};
        cfg.broker.address.uri = c.uri.c_str();
        cfg.broker.verification.crt_bundle_attach = attach_;
        if (!c.user.empty()) cfg.credentials.username = c.user.c_str();
        if (!c.password.empty()) cfg.credentials.authentication.password = c.password.c_str();
        cfg.credentials.client_id = c.clientId.c_str();
        cfg.session.last_will.topic = s->willTopic.c_str();
        cfg.session.last_will.msg = s->willPayload.c_str();
        cfg.session.last_will.msg_len = static_cast<int>(s->willPayload.size());
        cfg.session.last_will.qos = 1;
        cfg.session.last_will.retain = 1;
        cfg.session.keepalive = 30;
        cfg.network.reconnect_timeout_ms = 5000;
        cfg.outbox.limit = kOutboxLimit;
        s->handle = esp_mqtt_client_init(&cfg);
        if (!s->handle) {
            delete s;
            return 0;
        }
        esp_mqtt_client_register_event(s->handle, MQTT_EVENT_ANY, onEvent, s);
        if (esp_mqtt_client_start(s->handle) != ESP_OK) {
            esp_mqtt_client_destroy(s->handle);
            delete s;
            return 0;
        }
        session_ = s;
        return s->id;
    }

    void stop() override {
        Session* s = session_;
        session_ = nullptr;
        if (!s) return;
        s->live.store(false);
        // esp_mqtt_client_destroy() waits for esp-mqtt's task, which may be
        // in a connection attempt for seconds: not in the device's task.
        if (xTaskCreate(finishTask, "tk_mqtt_end", 4096, s, 3, nullptr) != pdPASS) finish(s);
    }

    bool publish(const std::string& topic, const std::string& payload, bool retain) override {
        Session* s = session_;
        if (!s || !s->connected.load()) return false;
        size_t bytes = topic.size() + payload.size();
        if (g_queuedBytes.load() + bytes > kQueueBytes) return false;
        Message* m = new Message{s, topic, payload, retain};
        s->queued.fetch_add(1);
        g_queuedBytes.fetch_add(bytes);
        if (xQueueSend(g_queue, &m, 0) != pdTRUE) {
            g_queuedBytes.fetch_sub(bytes);
            s->queued.fetch_sub(1);
            delete m;
            return false;
        }
        return true;
    }

private:
    esp_err_t (*attach_)(void*);
    Session* session_ = nullptr;  // the device's task's
    int32_t sessions_ = 0;
};

}  // namespace

MqttClient* espMqtt() {
    static EspMqtt client(nullptr);
    return &client;
}

// Only referenced through Mqtt::tls(), so the certificate bundle is linked
// only by applications that call it.
MqttClient* espMqttTls() {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    static EspMqtt client(esp_crt_bundle_attach);
    return &client;
#else
    return espMqtt();
#endif
}

}  // namespace tesserkit

#else  // no esp-mqtt in this build

namespace tesserkit {
MqttClient* espMqtt() { return nullptr; }
MqttClient* espMqttTls() { return nullptr; }
}  // namespace tesserkit

#endif
#endif
