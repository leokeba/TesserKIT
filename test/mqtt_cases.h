// Mqtt (docs/DESIGN.md section 5.8).
#pragma once

#include "rig.h"

namespace mqtt_cases {

using rig::at;
using rig::Op;
using rig::Status;

struct MqttRig : rig::Rig {
    tesserkit::Mqtt mqtt{dev};
    bool on = false;
    int brightness = 10;
    std::string name = "lamp";
    tesser::EventNode* pressed = nullptr;
    MqttRig() {
        auto& led = dev.api().object("led");
        led.value("on", on);
        led.value("brightness", brightness).range(0, 255);
        led.value("name", name);
        pressed = &led.event("pressed");
        led.value("secretive", brightness).writeAccess(tesser::Access::Admin);
        mqtt.broker("mqtt://broker.local:1883", "device", "pw").enable(true).publish("/led", 0);
    }
    std::string pre() const { return "tesser/" + dev.hostname(); }
    // Starts, and lets the broker accept the connection.
    void online() {
        CHECK(dev.begin());
        settle();
        fake.mqtt.connect();
        settle();
    }
};

TEST(mqtt_connects_with_its_settings) {
    MqttRig r;
    CHECK(r.dev.begin());
    r.settle();
    const tesserkit::MqttConfig& c = r.fake.mqtt.config;
    CHECK(r.fake.mqtt.running);
    CHECK_EQ(c.uri, "mqtt://broker.local:1883");
    CHECK_EQ(c.user, "device");
    CHECK_EQ(c.password, "pw");
    CHECK_EQ(c.clientId, r.dev.hostname());
    CHECK_EQ(c.willTopic, r.pre() + "/status");
    CHECK_EQ(c.willPayload, "offline");
    CHECK_EQ(c.subscriptions.size(), size_t(8));
    CHECK_EQ(c.subscriptions[0], r.pre() + "/+/set");
    CHECK_EQ(c.subscriptions[2], r.pre() + "/+/+/+/set");
    CHECK_EQ(r.value("/sys/mqtt/status/connected"), "false");
    r.fake.mqtt.connect();
    r.settle();
    CHECK_EQ(r.value("/sys/mqtt/status/connected"), "true");
    CHECK_EQ(r.fake.mqtt.retained[r.pre() + "/status"].payload, "online");
}

TEST(mqtt_waits_for_a_broker_and_enabled) {
    {
        struct Off : rig::Rig {
            tesserkit::Mqtt mqtt{dev};
        } r;
        r.mqtt.broker("mqtt://b");  // not enabled
        CHECK(r.dev.begin());
        r.settle();
        CHECK(!r.fake.mqtt.running);
    }
    struct NoBroker : rig::Rig {
        tesserkit::Mqtt mqtt{dev};
    } r;
    r.mqtt.enable(true);
    CHECK(r.dev.begin());
    r.settle();
    CHECK(!r.fake.mqtt.running);
    CHECK_EQ(r.set("/sys/mqtt/uri", "\"mqtt://b\"").status, Status::Ok);
    r.run(1100);  // the settings settle
    CHECK(r.fake.mqtt.running);
}

TEST(mqtt_publishes_leaves_retained) {
    MqttRig r;
    r.online();
    std::string p = r.pre();
    CHECK_EQ(r.fake.mqtt.retained[p + "/led/on"].payload, "false");
    CHECK_EQ(r.fake.mqtt.retained[p + "/led/brightness"].payload, "10");
    CHECK_EQ(r.fake.mqtt.retained[p + "/led/name"].payload, "\"lamp\"");
    CHECK_EQ(r.set("/led/brightness", "99").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.mqtt.last(p + "/led/brightness"), "99");
    CHECK(r.fake.mqtt.retained[p + "/led/brightness"].retain);
    r.pressed->emit(3);
    r.settle();
    CHECK_EQ(r.fake.mqtt.last(p + "/led/pressed"), "3");
    CHECK(r.fake.mqtt.retained.count(p + "/led/pressed") == 0);  // events aren't retained
    CHECK_EQ(r.value("/sys/mqtt/status/published"), std::to_string(r.fake.mqtt.published.size()));
}

TEST(mqtt_publishes_at_most_once_per_interval) {
    MqttRig r;
    r.mqtt.publish("/led/brightness", 1000);
    r.fake.mqtt.published.clear();
    CHECK(r.dev.begin());
    r.settle();
    r.fake.mqtt.connect();
    r.settle();
    size_t before = r.fake.mqtt.published.size();
    for (int i = 0; i < 5; i++) {
        CHECK_EQ(r.set("/led/brightness", std::to_string(20 + i)).status, Status::Ok);
        r.run(100);
    }
    r.run(1000);
    size_t n = 0;
    for (size_t i = before; i < r.fake.mqtt.published.size(); i++) {
        if (r.fake.mqtt.published[i].topic == r.pre() + "/led/brightness") n++;
    }
    // "/led" (interval 0) publishes each of the five; "/led/brightness" at most twice in 1.5 s.
    CHECK(n >= 6 && n <= 7);
    CHECK_EQ(r.fake.mqtt.last(r.pre() + "/led/brightness"), "24");
}

TEST(mqtt_commands_write_with_user_access) {
    MqttRig r;
    r.online();
    std::string p = r.pre();
    r.fake.mqtt.message(p + "/led/on/set", "true");
    r.settle();
    CHECK(r.on);
    CHECK_EQ(r.fake.mqtt.last(p + "/led/on/result"), "{\"status\":\"ok\",\"body\":true}");
    r.fake.mqtt.message(p + "/led/name/set", "desk lamp");  // plain text: a string
    r.settle();
    CHECK_EQ(r.name, "desk lamp");
    r.fake.mqtt.message(p + "/led/brightness/set", "300");
    r.settle();
    CHECK_EQ(at(r.fake.mqtt.last(p + "/led/brightness/result"), "status"), "\"invalid_value\"");
    r.fake.mqtt.message(p + "/led/secretive/set", "1");
    r.fake.mqtt.message(p + "/sys/mqtt/uri/set", "\"mqtt://evil\"");
    r.settle();
    CHECK_EQ(at(r.fake.mqtt.last(p + "/led/secretive/result"), "status"), "\"unauthorized\"");
    CHECK_EQ(at(r.fake.mqtt.last(p + "/sys/mqtt/uri/result"), "status"), "\"unauthorized\"");
    CHECK_EQ(r.fake.mqtt.config.uri, "mqtt://broker.local:1883");
    // Not ours.
    r.fake.mqtt.message("other/led/on/set", "false");
    r.fake.mqtt.message(p + "/set", "false");
    r.settle();
    CHECK(r.on);
}

// The broker's retained commands, all at once while the device's task is
// busy: the latest eight are kept, the others counted in status/missed.
TEST(mqtt_burst_of_commands_keeps_the_latest) {
    MqttRig r;
    r.online();
    std::string p = r.pre();
    CHECK_EQ(r.value("/sys/mqtt/status/missed"), "0");
    for (int i = 1; i <= 40; i++) r.fake.mqtt.message(p + "/led/brightness/set", std::to_string(i));
    r.settle();
    CHECK_EQ(r.brightness, 40);
    CHECK_EQ(r.value("/sys/mqtt/status/missed"), "32");
}

TEST(mqtt_follows_the_connection) {
    MqttRig r;
    r.online();
    CHECK_EQ(r.dev.api().subscriptionCount(), size_t(1));
    r.fake.mqtt.drop("broker gone");
    r.settle();
    CHECK_EQ(r.value("/sys/mqtt/status/connected"), "false");
    CHECK_EQ(r.value("/sys/mqtt/status/error"), "\"broker gone\"");
    CHECK_EQ(r.dev.api().subscriptionCount(), size_t(0));
    r.fake.mqtt.retained.clear();
    r.fake.mqtt.connect();  // the client reconnected by itself
    r.settle();
    CHECK_EQ(r.value("/sys/mqtt/status/error"), "\"\"");
    CHECK_EQ(r.fake.mqtt.retained[r.pre() + "/led/on"].payload, "false");  // published again
    CHECK_EQ(r.fake.mqtt.sessions, 1);
}

// The session's events go through the device's bounded queue, which can
// lose them: its state is read back once a second.
TEST(mqtt_lost_session_events_are_caught_up) {
    MqttRig r;
    CHECK(r.dev.begin());
    r.settle();
    tesserkit::EventSink real = r.fake.mqtt.sink;
    r.fake.mqtt.sink = [](tesserkit::Event) {};  // lost
    r.fake.mqtt.connect();
    r.run(1100);
    CHECK_EQ(r.value("/sys/mqtt/status/connected"), "true");
    CHECK_EQ(r.fake.mqtt.retained[r.pre() + "/status"].payload, "online");
    r.set("/led/on", "true");
    r.settle();
    CHECK_EQ(r.fake.mqtt.last(r.pre() + "/led/on"), "true");
    r.fake.mqtt.drop();
    r.run(1100);
    CHECK_EQ(r.value("/sys/mqtt/status/connected"), "false");
    r.fake.mqtt.sink = real;
}

TEST(mqtt_restarts_on_new_settings) {
    MqttRig r;
    r.online();
    CHECK_EQ(r.set("/sys/mqtt/prefix", "\"home/lamp\"").status, Status::Ok);
    r.run(500);
    CHECK_EQ(r.fake.mqtt.stops, 0);  // settling
    r.run(600);
    CHECK_EQ(r.fake.mqtt.stops, 1);
    CHECK_EQ(r.fake.mqtt.retained[r.pre() + "/status"].payload, "offline");  // the old session's will
    CHECK_EQ(r.fake.mqtt.sessions, 2);
    CHECK_EQ(r.fake.mqtt.config.willTopic, "home/lamp/status");
    CHECK_EQ(r.fake.mqtt.config.subscriptions[0], "home/lamp/+/set");
    r.fake.mqtt.connect();
    r.settle();
    CHECK_EQ(r.fake.mqtt.retained["home/lamp/led/on"].payload, "false");
    CHECK_EQ(r.set("/sys/mqtt/enabled", "false").status, Status::Ok);
    r.run(1100);
    CHECK(!r.fake.mqtt.running);
    CHECK_EQ(r.dev.api().subscriptionCount(), size_t(0));
}

TEST(mqtt_follows_a_rename) {
    // The default prefix and client ID are the hostname's: a rename restarts
    // the session, so the will and the "/set" subscriptions follow it, and
    // Home Assistant's configs move to the new hostname.
    struct Renamed : MqttRig {
        tesserkit::System sys{dev};
        Renamed() { mqtt.homeAssistant(true); }
    } r;
    r.online();
    r.run(200);
    std::string oldPre = r.pre(), oldHost = r.dev.hostname();
    std::string oldConfig = "homeassistant/switch/" + oldHost + "/led_on/config";
    CHECK(!r.fake.mqtt.retained[oldConfig].payload.empty());
    CHECK_EQ(r.set("/sys/name", "\"Kitchen\"").status, Status::Ok);
    CHECK_EQ(r.dev.hostname(), "kitchen");
    r.run(1100);  // the settings settle
    CHECK_EQ(r.fake.mqtt.sessions, 2);
    CHECK_EQ(r.fake.mqtt.retained[oldPre + "/status"].payload, "offline");  // the old session's will
    CHECK_EQ(r.fake.mqtt.config.clientId, "kitchen");
    CHECK_EQ(r.fake.mqtt.config.willTopic, "tesser/kitchen/status");
    CHECK_EQ(r.fake.mqtt.config.subscriptions[0], "tesser/kitchen/+/set");
    r.fake.mqtt.connect();
    r.settle();
    r.fake.mqtt.message("tesser/kitchen/led/on/set", "true");
    r.settle();
    CHECK(r.on);
    CHECK_EQ(r.fake.mqtt.last("tesser/kitchen/led/on/result"), "{\"status\":\"ok\",\"body\":true}");
    CHECK_EQ(r.fake.mqtt.retained["tesser/kitchen/led/on"].payload, "true");
    CHECK_EQ(r.fake.mqtt.retained[oldConfig].payload, "");  // emptied: removed from Home Assistant
    std::string sw = r.fake.mqtt.retained["homeassistant/switch/kitchen/led_on/config"].payload;
    CHECK_EQ(at(sw, "unique_id"), "\"kitchen_led_on\"");
    CHECK_EQ(at(sw, "command_topic"), "\"tesser/kitchen/led/on/set\"");
}

TEST(mqtt_rename_with_its_own_prefix) {
    // With a prefix and a client ID of its own, the session stays: only
    // discovery, whose topics and ids have the hostname, moves.
    struct Renamed : MqttRig {
        tesserkit::System sys{dev};
        Renamed() { mqtt.homeAssistant(true).prefix("home/lamp"); }
    } r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/mqtt/clientId", "\"lamp\"").status, Status::Ok);
    r.run(1100);
    r.fake.mqtt.connect();
    r.settle();
    int sessions = r.fake.mqtt.sessions;
    std::string oldConfig = "homeassistant/switch/" + r.dev.hostname() + "/led_on/config";
    CHECK(!r.fake.mqtt.retained[oldConfig].payload.empty());
    CHECK_EQ(r.set("/sys/name", "\"Kitchen\"").status, Status::Ok);
    r.run(1100);
    CHECK_EQ(r.fake.mqtt.sessions, sessions);
    CHECK_EQ(r.fake.mqtt.retained[oldConfig].payload, "");
    std::string sw = r.fake.mqtt.retained["homeassistant/switch/kitchen/led_on/config"].payload;
    CHECK_EQ(at(sw, "command_topic"), "\"home/lamp/led/on/set\"");
}

TEST(mqtt_ignores_an_old_session) {
    MqttRig r;
    CHECK(r.dev.begin());
    r.settle();
    tesserkit::Event late;
    late.type = tesserkit::Event::Type::MqttConnected;
    late.value = 7;  // not the session the client gave
    r.dev.post(late);
    r.settle();
    CHECK_EQ(r.value("/sys/mqtt/status/connected"), "false");
}

TEST(mqtt_new_publish_list_resubscribes) {
    MqttRig r;
    r.online();
    r.fake.mqtt.published.clear();
    CHECK_EQ(r.set("/sys/mqtt/publish", "[{\"path\":\"/led/on\",\"interval\":0}]").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.mqtt.last(r.pre() + "/led/on"), "false");
    CHECK_EQ(r.fake.mqtt.last(r.pre() + "/led/name"), "<none>");
    CHECK_EQ(r.fake.mqtt.sessions, 1);  // no reconnection
}

TEST(mqtt_counts_dropped_messages) {
    MqttRig r;
    r.fake.mqtt.full = true;
    r.online();
    CHECK(r.value("/sys/mqtt/status/dropped") != "0");
}

TEST(mqtt_settings_are_checked) {
    MqttRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/mqtt/uri", "\"http://x\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/mqtt/prefix", "\"a/#\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/mqtt/prefix", "\"a/\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/mqtt/publish", "[{\"path\":\"/nope\"}]").status, Status::NotFound);
    CHECK_EQ(r.set("/sys/mqtt/publish", "[{\"path\":\"/\"}]").status, Status::Ok);
    CHECK_EQ(r.value("/sys/mqtt/password"), "null");  // a secret
    CHECK_EQ(r.as(tesser::Access::User, Op::Set, "/sys/mqtt/enabled", "false").status, Status::Unauthorized);
}

TEST(mqtt_lost_sessions_are_events) {
    MqttRig r;
    r.online();
    rig::Events ev(r, "/sys/mqtt/lost");
    CHECK_EQ(at(r.schema("/sys/mqtt/lost").body, "ui/toast"), "\"warn\"");
    r.fake.mqtt.drop("broker gone");
    r.settle();
    std::vector<std::string> got = ev.take();
    CHECK_EQ(got.size(), size_t(1));
    if (!got.empty()) CHECK_EQ(got[0], "\"broker gone\"");
    // Failed retries aren't news, nor a session the settings stop.
    r.fake.mqtt.drop("still gone");
    r.settle();
    r.fake.mqtt.connect();
    r.settle();
    CHECK_EQ(r.value("/sys/mqtt/status/connected"), "true");
    CHECK_EQ(r.set("/sys/mqtt/enabled", "false").status, Status::Ok);
    r.run(1500);
    CHECK_EQ(r.value("/sys/mqtt/status/connected"), "false");
    CHECK(ev.take().empty());
}

// Home Assistant discovery.

struct HaRig : MqttRig {
    enum class Mode { Off, Heat, Cool } mode = Mode::Off;
    float temperature = 21.5f;
    int toggles = 0;
    HaRig() {
        auto& led = dev.api().object("led").label("LED");
        led.value("mode", mode, {"off", "heat", "cool"});
        led.value("temperature", static_cast<const float&>(temperature)).unit("°C").ui("precision", 1);
        led.action("toggle", [this] { toggles++; }).label("Toggle");
        led.action("blink", [](int times) { (void)times; });  // an argument: not a button
        mqtt.homeAssistant(true);
    }
    std::string config(const char* component, const char* id) {
        return fake.mqtt.retained["homeassistant/" + std::string(component) + "/" + dev.hostname() + "/" + id +
                                  "/config"]
            .payload;
    }
};

TEST(mqtt_discovery_announces_published_leaves) {
    HaRig r;
    r.online();
    r.run(200);  // a few per period
    std::string sw = r.config("switch", "led_on");
    CHECK_EQ(at(sw, "name"), "\"LED on\"");
    CHECK_EQ(at(sw, "unique_id"), "\"" + r.dev.hostname() + "_led_on\"");
    CHECK_EQ(at(sw, "state_topic"), "\"" + r.pre() + "/led/on\"");
    CHECK_EQ(at(sw, "command_topic"), "\"" + r.pre() + "/led/on/set\"");
    CHECK_EQ(at(sw, "payload_on"), "\"true\"");
    CHECK_EQ(at(sw, "availability_topic"), "\"" + r.pre() + "/status\"");
    CHECK_EQ(at(sw, "device/identifiers/0"), "\"" + r.dev.hostname() + "\"");
    CHECK_EQ(at(sw, "device/name"), "\"Test Device a67c\"");
    std::string num = r.config("number", "led_brightness");
    CHECK_EQ(at(num, "min"), "0");
    CHECK_EQ(at(num, "max"), "255");
    CHECK_EQ(at(num, "step"), "1");
    CHECK_EQ(at(num, "command_template"), "\"{{ value | int }}\"");
    CHECK_EQ(at(r.config("text", "led_name"), "command_template"), "\"{{ value | tojson }}\"");
    std::string sel = r.config("select", "led_mode");
    CHECK_EQ(at(sel, "options"), "[\"off\",\"heat\",\"cool\"]");
    CHECK_EQ(at(sel, "value_template"), "\"{{ value_json }}\"");
    std::string sensor = r.config("sensor", "led_temperature");
    CHECK_EQ(at(sensor, "unit_of_measurement"), "\"°C\"");
    CHECK_EQ(at(sensor, "suggested_display_precision"), "1");
    CHECK_EQ(at(sensor, "command_topic"), "<missing>");
    // Admin-only: the broker's clients can't write it, so a sensor.
    CHECK(r.config("sensor", "led_secretive") != "");
    CHECK(r.fake.mqtt.retained.count("homeassistant/number/" + r.dev.hostname() + "/led_secretive/config") == 0);
    std::string button = r.config("button", "led_toggle");
    CHECK_EQ(at(button, "payload_press"), "\"null\"");
    CHECK_EQ(at(button, "state_topic"), "<missing>");
    CHECK(r.fake.mqtt.retained.count("homeassistant/button/" + r.dev.hostname() + "/led_blink/config") == 0);
    CHECK_EQ(r.value("/sys/mqtt/status/entities"), "7");
    // The button works as Home Assistant presses it.
    r.fake.mqtt.message(r.pre() + "/led/toggle/set", "null");
    r.settle();
    CHECK_EQ(r.toggles, 1);
}

TEST(mqtt_discovery_waits_for_room) {
    HaRig r;
    r.online();
    r.fake.mqtt.full = true;
    CHECK_EQ(r.set("/sys/mqtt/publish", "[{\"path\":\"/led/on\",\"interval\":0}]").status, Status::Ok);
    r.fake.mqtt.retained.clear();
    r.settle();
    CHECK(r.config("switch", "led_on").empty());
    uint32_t dropped = static_cast<uint32_t>(std::stoul(r.value("/sys/mqtt/status/dropped")));
    r.fake.mqtt.full = false;
    r.settle();
    CHECK(!r.config("switch", "led_on").empty());
    // Waiting isn't dropping.
    CHECK_EQ(r.value("/sys/mqtt/status/dropped"), std::to_string(dropped));
}

TEST(mqtt_discovery_removes_what_goes) {
    HaRig r;
    r.online();
    r.run(200);
    std::string topic = "homeassistant/sensor/" + r.dev.hostname() + "/led_temperature/config";
    CHECK(!r.fake.mqtt.retained[topic].payload.empty());
    CHECK_EQ(r.set("/sys/mqtt/publish", "[{\"path\":\"/led/on\",\"interval\":0}]").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.mqtt.retained[topic].payload, "");  // emptied: removed from Home Assistant
    CHECK(!r.config("switch", "led_on").empty());
    CHECK_EQ(r.value("/sys/mqtt/status/entities"), "1");
    CHECK_EQ(r.set("/sys/mqtt/homeAssistant", "false").status, Status::Ok);
    r.settle();
    CHECK(r.config("switch", "led_on").empty());
    CHECK_EQ(r.value("/sys/mqtt/status/entities"), "0");
    CHECK_EQ(r.fake.mqtt.sessions, 1);  // no reconnection for any of it
}

TEST(mqtt_discovery_is_off_by_default) {
    MqttRig r;
    r.online();
    r.run(200);
    for (auto& kv : r.fake.mqtt.retained) CHECK(kv.first.compare(0, 14, "homeassistant/") != 0);
    CHECK_EQ(r.set("/sys/mqtt/discoveryPrefix", "\"\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/mqtt/discoveryPrefix", "\"ha/x\"").status, Status::Ok);
    CHECK_EQ(r.set("/sys/mqtt/homeAssistant", "true").status, Status::Ok);
    r.settle();
    CHECK(r.fake.mqtt.retained.count("ha/x/switch/" + r.dev.hostname() + "/led_on/config") == 1);
}

TEST(mqtt_tls_waits_for_memory) {
    // A TLS session's memory comes from TesserAPI's byte budget: refused, the
    // client doesn't start, and it is tried again later.
    MqttRig r;
    r.mqtt.broker("mqtts://broker.local:8883", "device", "pw");
    size_t before = tesser::memoryBudget().used();
    {
        rig::BudgetLimit low(20000);
        CHECK(r.dev.begin());
        r.settle();
        CHECK(!r.fake.mqtt.running);
        CHECK(r.value("/sys/mqtt/status/error").find("memory") != std::string::npos);
    }
    r.run(10100);
    CHECK(r.fake.mqtt.running);
    CHECK_EQ(tesser::memoryBudget().used(), before + tesserkit::kTlsBytes);  // held for the session
    r.fake.mqtt.connect();
    r.settle();
    CHECK_EQ(r.value("/sys/mqtt/status/connected"), "true");
    CHECK_EQ(r.set("/sys/mqtt/enabled", "false").status, Status::Ok);
    r.run(1100);
    CHECK(!r.fake.mqtt.running);
    CHECK_EQ(tesser::memoryBudget().used(), before);
    // Plain MQTT needs less: its tasks and buffers.
    {
        rig::BudgetLimit low(before + tesserkit::Mqtt::kSessionBytes + 1024);
        CHECK_EQ(r.set("/sys/mqtt/uri", "\"mqtt://broker.local:1883\"").status, Status::Ok);
        CHECK_EQ(r.set("/sys/mqtt/enabled", "true").status, Status::Ok);
        r.run(1100);
        CHECK(r.fake.mqtt.running);
        CHECK_EQ(tesser::memoryBudget().used(), before + tesserkit::Mqtt::kSessionBytes);
    }
}

TEST(mqtt_plain_sessions_wait_for_memory) {
    MqttRig r;
    size_t before = tesser::memoryBudget().used();
    {
        rig::BudgetLimit low(before + tesserkit::Mqtt::kSessionBytes / 2);
        CHECK(r.dev.begin());
        r.settle();
        CHECK(!r.fake.mqtt.running);
        CHECK(r.value("/sys/mqtt/status/error").find("memory") != std::string::npos);
    }
    r.run(10100);
    CHECK(r.fake.mqtt.running);
    CHECK_EQ(tesser::memoryBudget().used(), before + tesserkit::Mqtt::kSessionBytes);
}

TEST(mqtt_restart_waits_for_the_old_session) {
    // On a restart the old session tears down while the new one would
    // connect: its memory stays reserved until it has ended, so both can't
    // count on the same heap.
    MqttRig r;
    r.mqtt.broker("mqtts://broker.local:8883", "device", "pw");
    r.fake.mqtt.lingering = true;
    size_t before = tesser::memoryBudget().used();
    rig::BudgetLimit low(before + tesserkit::kTlsBytes + 1024);
    r.online();
    CHECK_EQ(r.value("/sys/mqtt/status/connected"), "true");
    CHECK_EQ(r.set("/sys/mqtt/prefix", "\"home/lamp\"").status, Status::Ok);
    r.run(1100);
    CHECK_EQ(r.fake.mqtt.stops, 1);
    CHECK_EQ(r.fake.mqtt.sessions, 1);  // the old one still holds its memory
    CHECK(r.value("/sys/mqtt/status/error").find("memory") != std::string::npos);
    r.fake.mqtt.ended();
    r.run(10100);
    CHECK_EQ(r.fake.mqtt.sessions, 2);
    CHECK_EQ(tesser::memoryBudget().used(), before + tesserkit::kTlsBytes);
}

}  // namespace mqtt_cases
