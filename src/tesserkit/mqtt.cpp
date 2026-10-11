#include "tesserkit/mqtt.h"

#include <string.h>

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

constexpr uint32_t kSettleMs = 1000;  // settings edited field by field: one restart
constexpr uint32_t kRetryMs = 10000;  // a client that couldn't start

bool startsWith(const std::string& s, const char* prefix) { return s.compare(0, strlen(prefix), prefix) == 0; }

}  // namespace

Mqtt& Mqtt::enable(bool on) {
    enabled_ = on;
    return *this;
}

Mqtt& Mqtt::broker(const char* uri, const char* user, const char* password) {
    uri_ = uri ? uri : "";
    user_ = user ? user : "";
    password_ = password ? password : "";
    return *this;
}

Mqtt& Mqtt::prefix(const char* p) {
    prefix_ = p ? p : "";
    return *this;
}

Mqtt& Mqtt::publish(const char* path, uint32_t intervalMs) {
    if (publish_.size() < kMaxPublish) publish_.push_back({path ? path : "", intervalMs});
    return *this;
}

Mqtt& Mqtt::tls() {
    Platform& p = dev_.platform();
    if (!p.mqtt) p.mqtt = espMqttTls();
    return *this;
}

std::string Mqtt::topicPrefix() const { return prefix_.empty() ? "tesser/" + dev_.hostname() : prefix_; }

std::string Mqtt::topic(const std::string& path) const {
    std::string t = topicPrefix();
    size_t start = 0;
    while (start < path.size() && path[start] == '/') start++;
    if (start < path.size()) t.append("/").append(path, start, std::string::npos);
    return t;
}

void Mqtt::describe(tesser::Object& o) {
    o.label("MQTT").doc("Publishes parts of the tree to a broker, and takes writes from it").writeAccess(Access::Admin);
    settings_.clear();
    settings_.push_back(&o.value("enabled", enabled_).persist());
    settings_.push_back(&o.value("uri", uri_)
                             .validate([](const std::string& v) {
                                 return v.empty() || startsWith(v, "mqtt://") || startsWith(v, "mqtts://") ||
                                                startsWith(v, "ws://") || startsWith(v, "wss://")
                                            ? Check::ok()
                                            : Check::fail(Status::InvalidValue, "mqtt://, mqtts://, ws:// or wss://");
                             })
                             .persist()
                             .label("Broker")
                             .ui("placeholder", "mqtt://broker.local:1883"));
    settings_.push_back(&o.value("user", user_).persist().label("User name"));
    settings_.push_back(&o.value("password", password_).secret().persist());
    settings_.push_back(
        &o.value("clientId", clientId_).persist().label("Client ID").ui("placeholder", "the hostname").ui("advanced"));
    settings_.push_back(&o.value("prefix", prefix_)
                             .validate([](const std::string& v) {
                                 bool ok = v.find_first_of("+#") == std::string::npos &&
                                           (v.empty() || (v.front() != '/' && v.back() != '/'));
                                 return ok ? Check::ok()
                                           : Check::fail(Status::InvalidValue, "no + or #, and no slash at either end");
                             })
                             .persist()
                             .label("Topic prefix")
                             .ui("placeholder", "tesser/<hostname>")
                             .ui("advanced"));
    publishNode_ = &o.list("publish", publish_,
                           [this](tesser::Object& e, MqttPublish& p) {
                               e.value("path", p.path)
                                   .validate([this](const std::string& v) {
                                       // Before the service starts the tree may be
                                       // incomplete: a boot load keeps what it stored.
                                       return v.empty() || !commander_ || v == "/" || resolves(dev_.api(), v)
                                                  ? Check::ok()
                                                  : Check::fail(Status::NotFound, "no such node");
                                   })
                                   .ui("placeholder", "/sys/status");
                               e.value("interval", p.interval).range(0, 86400000).unit("ms");
                           })
                        .maxSize(kMaxPublish)
                        .persist()
                        .label("Published")
                        .doc("Subtrees published as retained topics, one per leaf, at most once per interval");
    auto& s = o.object("status").label("Status");
    status_ = &s;
    s.value("connected", static_cast<const bool&>(connected_));
    s.value("error", static_cast<const std::string&>(error_)).label("Last error");
    s.value("published", static_cast<const uint32_t&>(published_)).label("Messages published").watch();
    s.value("dropped", static_cast<const uint32_t&>(dropped_)).label("Messages dropped").watch().ui("advanced");
}

uint32_t Mqtt::settingsGen() const {
    // Generations only grow, so the sum changes whenever one of them does.
    uint32_t sum = 0;
    for (const tesser::Node* n : settings_) sum += n->generation();
    return sum;
}

void Mqtt::start() {
    Platform& p = dev_.platform();
    if (!p.mqtt) p.mqtt = espMqtt();
    client_ = p.mqtt;
    if (!client_) tesser::logError("tesserkit: Mqtt has no MQTT client in this build");
    commander_.reset(new tesser::LocalClient(dev_.api()));
    commander_->setAccess(Access::User);  // the broker's clients aren't the device's admins
    settingsGen_ = settingsGen();
    publishGen_ = publishNode_->generation();
}

void Mqtt::tick(uint32_t nowMs) {
    if (commander_) commander_->process();
    for (Bridge& b : bridges_) b.client->process();
    uint32_t gen = settingsGen();
    if (gen != settingsGen_) {
        settingsGen_ = gen;
        restart_ = true;
        changedAtMs_ = nowMs;
    }
    if (restart_ && nowMs - changedAtMs_ >= kSettleMs) {
        restart_ = false;
        tried_ = false;
        disconnect();
    }
    bool wanted = enabled_ && !uri_.empty() && client_ && !restart_;
    bool online = dev_.state() == State::Online || dev_.onlineMs() > 0;
    if (!session_ && wanted && online && (!tried_ || nowMs - triedAtMs_ >= kRetryMs)) {
        tried_ = true;
        triedAtMs_ = nowMs;
        connect();
    }
    uint32_t pubGen = publishNode_->generation();
    if (pubGen != publishGen_) {
        publishGen_ = pubGen;
        if (connected_) {
            unbridge();
            bridge();
        }
    }
}

void Mqtt::event(const Event& e) {
    switch (e.type) {
        case Event::Type::MqttConnected:
            if (e.value != session_) return;
            connected_ = true;
            status_->changed();
            setError("");
            send(topicPrefix() + "/status", "online", true);
            bridge();
            return;
        case Event::Type::MqttDisconnected:
            if (e.value != session_) return;
            if (connected_) {
                connected_ = false;
                status_->changed();
                unbridge();
            }
            if (!e.text.empty()) setError(e.text);
            return;
        case Event::Type::MqttMessage:
            if (e.value == session_ && connected_) command(e.text, e.data);
            return;
        default:
            return;
    }
}

void Mqtt::connect() {
    MqttConfig c;
    c.uri = uri_;
    c.user = user_;
    c.password = password_;
    c.clientId = clientId_.empty() ? dev_.hostname() : clientId_;
    std::string pre = topicPrefix();
    c.willTopic = pre + "/status";
    c.willPayload = "offline";
    // "<prefix>/<path>/set" for paths 1 to kMaxSetDepth levels deep: a "#"
    // filter would bring back every retained topic published.
    std::string filter = pre;
    for (int i = 0; i < kMaxSetDepth; i++) {
        filter += "/+";
        c.subscriptions.push_back(filter + "/set");
    }
    Device& dev = dev_;
    session_ = client_->start(c, [&dev](Event ev) { dev.post(std::move(ev)); });
    if (!session_) setError("the MQTT client didn't start");
}

void Mqtt::disconnect() {
    unbridge();
    if (session_) client_->stop();
    session_ = 0;
    if (connected_) {
        connected_ = false;
        status_->changed();
    }
}

void Mqtt::bridge() {
    tesser::Api& api = dev_.api();
    for (const MqttPublish& p : publish_) {
        if (p.path.empty()) continue;
        Bridge b;
        b.path = p.path;
        b.client.reset(new tesser::LocalClient(api));  // Admin: what to publish is the admin's choice
        tesser::LocalClient* c = b.client.get();
        c->onNotification([this, c](JsonObjectConst env) {
            std::string path = env["path"] | "/";
            const char* op = env["op"] | "";
            if (strcmp(op, "event") == 0) {
                std::string payload;
                serializeJson(env["body"], payload);
                send(topic(path), payload, false);
                return;
            }
            publishTree(path, env["body"], true);
            // Changes were lost on the way: publish the whole subtree again.
            if (env["overflow"] | false) {
                c->get(path, [this, path](Status s, JsonVariantConst body) {
                    if (s == Status::Ok) publishTree(path, body, true);
                });
            }
        });
        tesser::Query q;
        q.interval = p.interval;
        std::string path = p.path;
        c->subscribe(
            path,
            [this, path](Status s, JsonVariantConst body) {
                if (s == Status::Ok) return publishTree(path, body, true);
                setError("publishing " + path + ": " + tesser::toString(s));
            },
            q);
        bridges_.push_back(std::move(b));
    }
}

void Mqtt::unbridge() { bridges_.clear(); }  // their subscriptions go with them

void Mqtt::publishTree(const std::string& path, JsonVariantConst v, bool retain) {
    if (v.is<JsonObjectConst>()) {
        for (JsonPairConst kv : v.as<JsonObjectConst>()) {
            std::string child = path;
            if (child.empty() || child.back() != '/') child += '/';
            child += kv.key().c_str();
            publishTree(child, kv.value(), retain);
        }
        return;
    }
    std::string payload;
    serializeJson(v, payload);
    send(topic(path), payload, retain);
}

void Mqtt::send(const std::string& t, const std::string& payload, bool retain) {
    if (!client_ || !session_) return;
    if (client_->publish(t, payload, retain)) {
        published_++;
    } else {
        dropped_++;
    }
}

void Mqtt::command(const std::string& t, const std::string& payload) {
    std::string pre = topicPrefix() + "/";
    static constexpr size_t kSet = 4;  // "/set"
    if (t.size() <= pre.size() + kSet || t.compare(0, pre.size(), pre) != 0 ||
        t.compare(t.size() - kSet, kSet, "/set") != 0) {
        return;
    }
    std::string rel = t.substr(pre.size(), t.size() - pre.size() - kSet);
    std::string body = payload;
    JsonDocument doc;
    if (payload.empty()) {
        body = "null";  // an action without argument
    } else if (deserializeJson(doc, payload)) {
        // Not JSON: plain text is a string ("ON", a name).
        doc.clear();
        doc.set(payload);
        body.clear();
        serializeJson(doc, body);
    }
    std::string result = pre + rel + "/result";
    commander_->set("/" + rel, body, [this, result](Status s, JsonVariantConst b) {
        JsonDocument r;
        r["status"] = tesser::toString(s);
        r["body"] = b;
        std::string out;
        serializeJson(r, out);
        send(result, out, false);
    });
}

void Mqtt::setError(const std::string& e) {
    if (e == error_) return;
    if (!e.empty()) tesser::logWarning("tesserkit: MQTT: %s", e.c_str());
    error_ = e;
    status_->changed();
}

}  // namespace tesserkit
