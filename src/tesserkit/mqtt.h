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

    void describe(tesser::Object& o) override;

    bool connected() const { return connected_; }
    // The topics' prefix: the setting, or "tesser/<hostname>".
    std::string topicPrefix() const;
    // The topic of a path: "<prefix>/a/b" for "/a/b".
    std::string topic(const std::string& path) const;

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
    void bridge();
    void unbridge();
    void publishTree(const std::string& path, JsonVariantConst v, bool retain);
    void send(const std::string& topic, const std::string& payload, bool retain);
    void command(const std::string& topic, const std::string& payload);
    void setError(const std::string& e);

    bool enabled_ = false;
    std::string uri_, user_, password_, clientId_, prefix_;
    std::vector<MqttPublish> publish_;

    // Status.
    bool connected_ = false;
    std::string error_;
    uint32_t published_ = 0, dropped_ = 0;
    tesser::Node* status_ = nullptr;
    tesser::EventNode* lost_ = nullptr;

    std::vector<tesser::Node*> settings_;  // a change restarts the session
    tesser::Node* publishNode_ = nullptr;
    uint32_t settingsGen_ = 0, publishGen_ = 0;
    uint32_t changedAtMs_ = 0;
    bool restart_ = false;  // settings changed: restart once they settle
    bool tried_ = false;
    uint32_t triedAtMs_ = 0;
    MqttClient* client_ = nullptr;
    int32_t session_ = 0;
    std::vector<Bridge> bridges_;
    std::unique_ptr<tesser::LocalClient> commander_;
};

}  // namespace tesserkit
