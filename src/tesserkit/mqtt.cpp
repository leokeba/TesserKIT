#include "tesserkit/mqtt.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

constexpr uint32_t kSettleMs = 1000;  // settings edited field by field: one restart
constexpr uint32_t kRetryMs = 10000;  // a client that couldn't start
constexpr int kAnnouncePerTick = 4;   // discovery shares the queue with the states

bool startsWith(const std::string& s, const char* prefix) { return s.compare(0, strlen(prefix), prefix) == 0; }

Check topicPart(const std::string& v) {
    bool ok = v.find_first_of("+#") == std::string::npos && (v.empty() || (v.front() != '/' && v.back() != '/'));
    return ok ? Check::ok() : Check::fail(Status::InvalidValue, "no + or #, and no slash at either end");
}

#if !defined(TESSERKIT_NO_HOME_ASSISTANT)
// Presentation metadata, where the build keeps it (not with TESSER_NO_UI).
const tesser::UiMeta* uiOf(const tesser::Node& n) { return n.meta() ? n.meta()->ui : nullptr; }

const char* labelOf(const tesser::Node& n) {
    const tesser::UiMeta* ui = uiOf(n);
    return ui && ui->label ? ui->label : n.name();
}

const tesser::UiHint* hintOf(const tesser::Node& n, const char* key) {
    const tesser::UiMeta* ui = uiOf(n);
    for (const tesser::UiHint* h = ui ? ui->hints : nullptr; h; h = h->next) {
        if (strcmp(h->key, key) == 0) return h;
    }
    return nullptr;
}

// Home Assistant's object id for a path: "/led/on" → "led_on".
std::string objectId(const std::string& path) {
    std::string id;
    for (char c : path) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (ok) {
            id.push_back(c);
        } else if (!id.empty() && id.back() != '_') {
            id.push_back('_');
        }
    }
    while (!id.empty() && id.back() == '_') id.pop_back();
    return id;
}

// The Home Assistant component a node becomes, or null (docs/DESIGN.md
// section 5.8). `writable`: the broker's clients (User) may write it.
const char* componentOf(const tesser::Node& n, bool writable) {
    if (hintOf(n, "hidden")) return nullptr;
    if (n.type() == tesser::NodeType::Action) {
        return writable && !static_cast<const tesser::ActionNode&>(n).argKind() ? "button" : nullptr;
    }
    if (n.type() != tesser::NodeType::Value) return nullptr;
    const auto& v = static_cast<const tesser::ValueNode&>(n);
    if (v.isSecret()) return nullptr;
    writable = writable && v.writable();
    switch (v.kind()) {
        case tesser::ValueKind::Boolean: return writable ? "switch" : "binary_sensor";
        case tesser::ValueKind::Integer:
        case tesser::ValueKind::Number: return writable ? "number" : "sensor";
        case tesser::ValueKind::String:
            if (!writable) return "sensor";
            return v.options() ? "select" : "text";
    }
    return nullptr;
}

#endif
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

Mqtt& Mqtt::homeAssistant(bool on, const char* discoveryPrefix) {
    homeAssistant_ = on;
    discoveryPrefix_ = discoveryPrefix ? discoveryPrefix : "";
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
    o.label("MQTT").ui("icon", "link").doc("Publishes parts of the tree to a broker, and takes writes from it").writeAccess(Access::Admin);
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
                             .validate(topicPart)
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
    discoverySettings_.clear();
#if !defined(TESSERKIT_NO_HOME_ASSISTANT)
    discoverySettings_.push_back(&o.value("homeAssistant", homeAssistant_)
                                      .persist()
                                      .label("Home Assistant")
                                      .doc("Announces the published values and actions to Home Assistant (MQTT discovery)"));
    discoverySettings_.push_back(&o.value("discoveryPrefix", discoveryPrefix_)
                                      .validate([](const std::string& v) {
                                          return v.empty() ? Check::fail(Status::InvalidValue, "empty") : topicPart(v);
                                      })
                                      .persist()
                                      .label("Discovery prefix")
                                      .ui("visibleWhen", "homeAssistant=true")
                                      .ui("advanced"));
    discoverySettings_.push_back(publishNode_);
#endif
    auto& s = o.object("status").label("Status");
    status_ = &s;
    s.value("connected", static_cast<const bool&>(connected_));
    s.value("error", static_cast<const std::string&>(error_)).label("Last error");
    s.value("published", static_cast<const uint32_t&>(published_)).label("Messages published").watch();
    s.value("dropped", static_cast<const uint32_t&>(dropped_)).label("Messages dropped").watch().ui("advanced");
    s.value("missed", static_cast<const uint32_t&>(missed_))
        .label("Commands missed")
        .doc("Received while the device's queue was full, and dropped")
        .watch()
        .ui("advanced");
#if !defined(TESSERKIT_NO_HOME_ASSISTANT)
    entitiesNode_ = &s.value("entities", static_cast<const uint32_t&>(entities_))
                         .label("Home Assistant entities")
                         .ui("visibleWhen", "../homeAssistant=true");
#endif
    lost_ = &o.event("lost").label("Connection lost").ui("toast", "warn").ui("hidden");
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
    discoveryGen_ = 0;
    for (const tesser::Node* n : discoverySettings_) discoveryGen_ += n->generation();
}

void Mqtt::tick(uint32_t nowMs) {
    missed_ = dev_.mqttMessagesDropped();
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
    uint32_t discGen = 0;
    for (const tesser::Node* n : discoverySettings_) discGen += n->generation();
    if (discGen != discoveryGen_) {
        discoveryGen_ = discGen;
        if (connected_) discover();
    }
    if (connected_) announce();
}

void Mqtt::event(const Event& e) {
    switch (e.type) {
        case Event::Type::MqttConnected:
            if (e.value != session_) return;
            tls_.settle();  // the session's buffers are allocated: the heap shows them
            connected_ = true;
            status_->changed();
            setError("");
            send(topicPrefix() + "/status", "online", true);
            bridge();
            discover();
            return;
        case Event::Type::MqttDisconnected:
            if (e.value != session_) return;
            // The attempt is over: what it took isn't about to be taken.
            tls_.settle();
            if (!e.text.empty()) setError(e.text);
            if (connected_) {
                connected_ = false;
                status_->changed();
                unbridge();
                // Not stopped here: the broker or the network went.
                if (e.text.empty()) {
                    lost_->emit();
                } else {
                    lost_->emit(e.text);
                }
            }
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
    // A TLS session's memory, from the byte budget: refused, the client
    // isn't started, and the next try comes after kRetryMs.
    if (!reserveTls(uri_, tls_)) {
        setError("not enough free memory for TLS now (about 44 KB needed)");
        return;
    }
    Device& dev = dev_;
    session_ = client_->start(c, [&dev](Event ev) { dev.post(std::move(ev)); });
    if (!session_) {
        tls_.release();
        setError("the MQTT client didn't start");
    }
}

void Mqtt::disconnect() {
    unbridge();
    if (session_) client_->stop();
    session_ = 0;
    tls_.release();
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

namespace tesserkit {

#if defined(TESSERKIT_NO_HOME_ASSISTANT)

// Left out of the build (CONFIG_TESSERKIT_HOME_ASSISTANT=n).
void Mqtt::discover() {}
void Mqtt::announce() {}

#else

// Home Assistant discovery (docs/DESIGN.md section 5.8): a retained config
// per published value or action, under
// <discoveryPrefix>/<component>/<hostname>/<object id>/config.

const tesser::Node* Mqtt::resolve(const std::string& path, Access& write, const tesser::Node** parent) const {
    const tesser::Node* node = &dev_.api();
    write = node->writeAccess();
    *parent = nullptr;
    size_t start = !path.empty() && path[0] == '/' ? 1 : 0;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        if (node->type() != tesser::NodeType::Object) return nullptr;
        *parent = node;
        node = static_cast<const tesser::Object*>(node)->child(std::string_view(path).substr(start, end - start));
        if (!node) return nullptr;
        if (node->writeAccess() > write) write = node->writeAccess();
        start = end + 1;
    }
    return node;
}

void Mqtt::collect(const tesser::Node& n, const std::string& path, Access write,
                   std::vector<std::string>& out) const {
    if (out.size() >= kMaxEntities) return;
    if (n.type() == tesser::NodeType::Object) {
        if (hintOf(n, "hidden")) return;
        for (const tesser::Node* c = static_cast<const tesser::Object&>(n).first(); c; c = c->next()) {
            Access w = c->writeAccess() > write ? c->writeAccess() : write;
            collect(*c, path == "/" ? "/" + std::string(c->name()) : path + "/" + c->name(), w, out);
        }
        return;
    }
    if (componentOf(n, write <= Access::User)) out.push_back(path);
}

std::string Mqtt::discoveryTopic(const std::string& path, const tesser::Node& n, Access write) const {
    return discoveryPrefix_ + "/" + componentOf(n, write <= Access::User) + "/" + dev_.hostname() + "/" +
           objectId(path) + "/config";
}

std::string Mqtt::discoveryConfig(const std::string& path, const tesser::Node& n, Access write,
                                  const tesser::Node* parent) const {
    bool writable = write <= Access::User;
    std::string component = componentOf(n, writable);
    writable = component != "sensor" && component != "binary_sensor";
    JsonDocument doc;
    std::string name = labelOf(n);
    if (parent && parent != &dev_.api()) name = std::string(labelOf(*parent)) + " " + name;
    doc["name"] = name;
    doc["unique_id"] = dev_.hostname() + "_" + objectId(path);
    if (component != "button") doc["state_topic"] = topic(path);
    if (writable) doc["command_topic"] = topic(path) + "/set";
    doc["availability_topic"] = topicPrefix() + "/status";
    doc["payload_available"] = "online";
    doc["payload_not_available"] = "offline";
    if (startsWith(path, "/sys/")) doc["entity_category"] = writable ? "config" : "diagnostic";
    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"].to<JsonArray>().add(dev_.hostname());
    device["name"] = dev_.label();
    device["manufacturer"] = "TesserKIT";
    if (!dev_.info().chip.empty()) device["model"] = dev_.info().chip;
    if (!dev_.version().empty()) device["sw_version"] = dev_.version();
    if (uint32_t ip = dev_.address()) {
        char url[32];
        snprintf(url, sizeof(url), "http://%u.%u.%u.%u/", static_cast<unsigned>(ip >> 24),
                 static_cast<unsigned>((ip >> 16) & 0xFF), static_cast<unsigned>((ip >> 8) & 0xFF),
                 static_cast<unsigned>(ip & 0xFF));
        device["configuration_url"] = url;
    }
    if (component == "button") {
        doc["payload_press"] = "null";  // an action without argument
        std::string out;
        serializeJson(doc, out);
        return out;
    }
    const auto& v = static_cast<const tesser::ValueNode&>(n);
    const tesser::UiMeta* ui = uiOf(n);
    const tesser::NodeMeta* meta = n.meta();
    bool numeric = v.kind() == tesser::ValueKind::Integer || v.kind() == tesser::ValueKind::Number;
    if (ui && ui->unit && numeric) doc["unit_of_measurement"] = ui->unit;
    if (component == "switch" || component == "binary_sensor") {
        // Payloads are JSON: true and false.
        doc["payload_on"] = "true";
        doc["payload_off"] = "false";
        if (component == "switch") {
            doc["state_on"] = "true";
            doc["state_off"] = "false";
        }
    } else if (component == "number") {
        bool ranged = meta && meta->hasRange;
        doc["min"] = ranged ? meta->min : -1e9;
        doc["max"] = ranged ? meta->max : 1e9;
        double step = ui && ui->step > 0 ? static_cast<double>(ui->step) : v.kind() == tesser::ValueKind::Integer ? 1 : 0.001;
        doc["step"] = step;
        doc["mode"] = ranged ? "slider" : "box";
        if (v.kind() == tesser::ValueKind::Integer) doc["command_template"] = "{{ value | int }}";
    } else if (component == "sensor") {
        if (numeric) {
            doc["state_class"] = "measurement";
        } else {
            doc["value_template"] = "{{ value_json }}";
            if (const auto* opts = v.options()) {
                doc["device_class"] = "enum";
                JsonArray a = doc["options"].to<JsonArray>();
                for (const char* o : *opts) a.add(o);
            }
        }
    } else {  // select, text: JSON strings both ways
        doc["value_template"] = "{{ value_json }}";
        doc["command_template"] = "{{ value | tojson }}";
        if (const auto* opts = v.options()) {
            JsonArray a = doc["options"].to<JsonArray>();
            for (const char* o : *opts) a.add(o);
        } else if (v.maxLength() > 0 && v.maxLength() <= 255) {
            doc["max"] = static_cast<uint32_t>(v.maxLength());
        }
    }
    if (const tesser::UiHint* h = hintOf(n, "precision")) {
        if (h->kind == tesser::UiHint::Kind::Number) doc["suggested_display_precision"] = static_cast<int>(h->num);
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}

void Mqtt::discover() {
    std::vector<std::string> paths;
    if (homeAssistant_) {
        for (const MqttPublish& p : publish_) {
            if (p.path.empty()) continue;
            Access write;
            const tesser::Node* parent;
            const tesser::Node* n = resolve(p.path, write, &parent);
            std::string path = p.path.size() > 1 && p.path.back() == '/' ? p.path.substr(0, p.path.size() - 1) : p.path;
            if (n) collect(*n, path, write, paths);
        }
        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    }
    std::vector<std::string> topics;
    for (const std::string& path : paths) {
        Access write;
        const tesser::Node* parent;
        if (const tesser::Node* n = resolve(path, write, &parent)) topics.push_back(discoveryTopic(path, *n, write));
    }
    // What was announced and isn't any more: emptied, which removes it.
    for (const std::string& t : announced_) {
        if (std::find(topics.begin(), topics.end(), t) == topics.end() &&
            std::find(clearing_.begin(), clearing_.end(), t) == clearing_.end()) {
            clearing_.push_back(t);
        }
    }
    announced_ = topics;
    pending_ = paths;
    if (entities_ != topics.size()) {
        entities_ = static_cast<uint32_t>(topics.size());
        entitiesNode_->changed();
    }
}

void Mqtt::announce() {
    // A config at a time, while the client's queue takes them: discovery
    // never pushes the states out.
    for (int i = 0; i < kAnnouncePerTick && client_ && session_; i++) {
        if (!clearing_.empty()) {
            if (!client_->publish(clearing_.front(), "", true)) return;
            published_++;
            clearing_.erase(clearing_.begin());
            continue;
        }
        if (pending_.empty()) return;
        Access write;
        const tesser::Node* parent;
        const std::string& path = pending_.front();
        if (const tesser::Node* n = resolve(path, write, &parent)) {
            if (!client_->publish(discoveryTopic(path, *n, write), discoveryConfig(path, *n, write, parent), true)) return;
            published_++;
        }
        pending_.erase(pending_.begin());
    }
}

#endif

}  // namespace tesserkit
