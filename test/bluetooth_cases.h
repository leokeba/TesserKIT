// Bluetooth (docs/DESIGN.md section 5.10).
#pragma once

#include "rig.h"

namespace bluetooth_cases {

using rig::Status;

struct BluetoothRig : rig::Rig {
    tesserkit::Network net{dev};
    tesserkit::Bluetooth bt{dev};
};

TEST(bluetooth_advertises_the_hostname) {
    BluetoothRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.fake.ble.started);
    CHECK(r.fake.ble.advertising);
    CHECK_EQ(r.fake.ble.advertised, r.dev.hostname());
    CHECK_EQ(r.value("/sys/bluetooth/name"), "\"" + r.dev.hostname() + "\"");
    // The hostname changes: so does the name, without a restart.
    CHECK_EQ(r.set("/sys/network/hostname", "\"helio-2\"").status, Status::Ok);
    r.run(600);
    CHECK_EQ(r.fake.ble.advertised, "helio-2");
    CHECK_EQ(r.value("/sys/bluetooth/name"), "\"helio-2\"");
    CHECK_EQ(r.fake.ble.begins, 1);
}

TEST(bluetooth_shows_its_client) {
    BluetoothRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.value("/sys/bluetooth/status/connected"), "false");
    r.fake.ble.connectedNow = true;
    r.fake.ble.clientAddress = "aa:bb:cc:dd:ee:ff";
    r.run(600);
    CHECK_EQ(r.value("/sys/bluetooth/status/connected"), "true");
    CHECK_EQ(r.value("/sys/bluetooth/status/client"), "\"aa:bb:cc:dd:ee:ff\"");
    r.fake.ble.connectedNow = false;
    r.run(600);
    CHECK_EQ(r.value("/sys/bluetooth/status/client"), "\"\"");
}

TEST(bluetooth_switched_off_gives_its_memory_back) {
    BluetoothRig r;
    CHECK(r.dev.begin());
    r.settle();
    r.fake.ble.connectedNow = true;
    r.run(600);
    CHECK_EQ(r.value("/sys/bluetooth/status/connected"), "true");
    CHECK_EQ(r.set("/sys/bluetooth/enabled", "false").status, Status::Ok);
    r.run(600);
    CHECK_EQ(r.fake.ble.ends, 1);  // the stack is stopped, not only silent
    CHECK(!r.fake.ble.started);
    CHECK(!r.fake.ble.connectedNow);  // the client is dropped
    CHECK_EQ(r.value("/sys/bluetooth/status/connected"), "false");
    CHECK_EQ(r.value("/sys/bluetooth/restartNeeded"), "false");
    CHECK_EQ(r.set("/sys/bluetooth/enabled", "true").status, Status::Ok);
    r.settle();
    CHECK(r.fake.ble.advertising);
    CHECK_EQ(r.fake.ble.begins, 2);
}

TEST(bluetooth_off_without_release_stops_advertising) {
    BluetoothRig r;
    r.fake.ble.releasable = false;  // Arduino's library
    CHECK(r.dev.begin());
    r.settle();
    r.fake.ble.connectedNow = true;
    CHECK_EQ(r.set("/sys/bluetooth/enabled", "false").status, Status::Ok);
    r.settle();
    CHECK(r.fake.ble.started);
    CHECK(!r.fake.ble.advertising);
    CHECK(!r.fake.ble.connectedNow);
    CHECK_EQ(r.value("/sys/bluetooth/restartNeeded"), "false");
    CHECK_EQ(r.set("/sys/bluetooth/enabled", "true").status, Status::Ok);
    r.settle();
    CHECK(r.fake.ble.advertising);
    CHECK_EQ(r.fake.ble.begins, 1);
}

TEST(bluetooth_off_at_boot_gives_its_memory_to_the_heap) {
    BluetoothRig r;
    r.bt.enable(false);
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.fake.ble.released);
    CHECK(!r.fake.ble.started);
    CHECK_EQ(r.value("/sys/bluetooth/restartNeeded"), "false");
    // Switched on: nothing to start with until a restart, and the warning
    // says why.
    CHECK_EQ(r.set("/sys/bluetooth/enabled", "true").status, Status::Ok);
    r.settle();
    CHECK(!r.fake.ble.started);
    CHECK_EQ(r.fake.ble.begins, 0);
    CHECK_EQ(r.value("/sys/bluetooth/restartNeeded"), "true");
    CHECK(r.value("/sys/bluetooth/restartWarning").find("off at boot") != std::string::npos);
    // Off again: nothing waits.
    CHECK_EQ(r.set("/sys/bluetooth/enabled", "false").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.value("/sys/bluetooth/restartNeeded"), "false");
}

TEST(bluetooth_on_at_boot_keeps_its_memory) {
    BluetoothRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK(!r.fake.ble.released);
    CHECK(r.value("/sys/bluetooth/restartWarning").find("switched off while running") != std::string::npos);
}

TEST(bluetooth_off_at_boot_without_release_starts_when_enabled) {
    BluetoothRig r;
    r.fake.ble.bootRelease = false;  // Arduino's library
    r.bt.enable(false);
    CHECK(r.dev.begin());
    r.settle();
    CHECK(!r.fake.ble.started);
    CHECK_EQ(r.set("/sys/bluetooth/enabled", "true").status, Status::Ok);
    r.settle();
    CHECK(r.fake.ble.started);
    CHECK_EQ(r.fake.ble.begins, 1);
    CHECK_EQ(r.value("/sys/bluetooth/restartNeeded"), "false");
}

TEST(bluetooth_without_runtime_control_waits_for_a_restart) {
    BluetoothRig r;
    r.fake.ble.runtime = false;
    r.fake.ble.releasable = false;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.set("/sys/bluetooth/enabled", "false").status, Status::Ok);
    r.settle();
    CHECK(r.fake.ble.advertising);
    CHECK_EQ(r.value("/sys/bluetooth/restartNeeded"), "true");
    // Back to what runs: nothing to wait for.
    CHECK_EQ(r.set("/sys/bluetooth/enabled", "true").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.value("/sys/bluetooth/restartNeeded"), "false");
}

TEST(bluetooth_settings_need_admin) {
    BluetoothRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.as(tesser::Access::User, rig::Op::Set, "/sys/bluetooth/enabled", "false").status,
             Status::Unauthorized);
}

}  // namespace bluetooth_cases
