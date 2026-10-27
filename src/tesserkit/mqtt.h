#pragma once

#include <memory>
#include <string>
#include <vector>

#include "tesser/client.h"
#include "tesserkit/device.h"

namespace tesserkit {

// An element of /sys/mqtt/publish: a subtree published to the broker.
struct MqttPublish {
    std::string path;
    uint32_t interval = 1000;  // ms: at most one update per leaf
};

// /sys/mqtt: a bridge between the tree and an MQTT broker (docs/DESIGN.md
// section 5.8). Published subtrees go out as retained topics, one per leaf;
// "<prefix>/<path>/set" messages come back in as writes, with User access.
class Mqtt : public Service {
public:
    static constexpr size_t kMaxPublish = 8;
    // The deepest path a "/set" topic can address.
    static constexpr int kMaxSetDepth = 8;

    explicit Mqtt(Device& dev) : Service(dev, "mqtt") {}

    // Factory defaults, before Device::begin().
    Mqtt& enable(bool on);
    Mqtt& broker(const char* uri, const char* user = "", const char* password = "");
    Mqtt& prefix(const char* prefix);
    Mqtt& publish(const char* path, uint32_t intervalMs = 1000);
    // Verifies mqtts:// and wss:// brokers with ESP-IDF's certificate
    // bundle, linked only by applications that call it.
    Mqtt& tls();
    // Announces the published leaves to Home Assistant (MQTT discovery),
    // under `discoveryPrefix`.
    Mqtt& homeAssistant(bool on, const char* discoveryPrefix = "homeassistant");

    void describe(tesser::Object& o) override;

    bool connected() const { return connected_; }
    // The topics' prefix: the setting, or "tesser/<hostname>".
    std::string topicPrefix() const;
    // The topic of a path: "<prefix>/a/b" for "/a/b".
    std::string topic(const std::string& path) const;
    // The most entities announced to Home Assistant.
    static constexpr size_t kMaxEntities = 64;
    // What a plain session (mqtt://, ws://) takes from the heap, reserved
    // from TesserAPI's byte budget before it starts: esp-mqtt's task (6 KB
    // of stack), the publishing task (4 KB) and the client's two 1 KB
    // buffers. A TLS session reserves kTlsBytes instead.
    static constexpr size_t kSessionBytes = 12 * 1024;

protected:
    void start() override;
    void tick(uint32_t nowMs) override;
    void event(const Event& e) override;

private:
    struct Bridge {
        std::string path;
        std::unique_ptr<tesser::LocalClient> client;
    };

    uint32_t settingsGen() const;
    void connect();
    void disconnect();
    void onConnected();
    void onDisconnected(const std::string& why);
    void bridge();
    void unbridge();
    void publishTree(const std::string& path, JsonVariantConst v, bool retain);
    void send(std::string topic, std::string payload, bool retain);
    void command(const std::string& topic, const std::string& payload);
    void setError(const std::string& e);
    // Home Assistant discovery.
    void discover();
    void collect(const tesser::Node& n, const std::string& path, tesser::Access write,
                 std::vector<std::string>& out) const;
    std::string discoveryTopic(const std::string& path, const tesser::Node& n, tesser::Access write) const;
    std::string discoveryConfig(const std::string& path, const tesser::Node& n, tesser::Access write,
                                const tesser::Node* parent) const;
    const tesser::Node* resolve(const std::string& path, tesser::Access& write, const tesser::Node** parent) const;
    void announce();

    bool enabled_ = false;
    std::string uri_, user_, password_, clientId_, prefix_;
    std::vector<MqttPublish> publish_;
    bool homeAssistant_ = false;
    std::string discoveryPrefix_ = "homeassistant";

    // Status.
    bool connected_ = false;
    std::string error_;
    uint32_t published_ = 0, dropped_ = 0;
    uint32_t missed_ = 0;  // Device::mqttMessagesDropped()
    uint32_t entities_ = 0;
    tesser::Node* entitiesNode_ = nullptr;
    tesser::Node* status_ = nullptr;
    tesser::EventNode* lost_ = nullptr;

    std::vector<tesser::Node*> settings_;  // a change restarts the session
    tesser::Node* publishNode_ = nullptr;
    uint32_t settingsGen_ = 0, publishGen_ = 0;
    uint32_t hostnameGen_ = 0;  // Device::hostnameGeneration(): topics and ids follow it
    uint32_t changedAtMs_ = 0;
    bool restart_ = false;  // settings changed: restart once they settle
    bool tried_ = false;
    uint32_t triedAtMs_ = 0;
    uint32_t checkedAtMs_ = 0;  // the session's state, read back
    MqttClient* client_ = nullptr;
    int32_t session_ = 0;
    std::vector<Bridge> bridges_;
    std::unique_ptr<tesser::LocalClient> commander_;
    // Discovery: the paths still to announce, the config topics still to
    // empty, and those announced (kept across sessions, to empty them when
    // they go).
    std::vector<tesser::Node*> discoverySettings_;
    uint32_t discoveryGen_ = 0;
    std::vector<std::string> pending_;
    std::vector<std::string> clearing_;
    std::vector<std::string> announced_;
};

}  // namespace tesserkit
