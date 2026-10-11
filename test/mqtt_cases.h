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

}  // namespace mqtt_cases
