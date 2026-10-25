// Board (docs/DESIGN.md section 5.24): the status LED and the function
// button, on the fake platform's pins.
#pragma once

#include <vector>

#include "rig.h"

namespace board_cases {

using tesserkit::Board;
using tesserkit::State;
using tesser::Status;

constexpr int kLed = 2;
constexpr int kButton = 17;

struct BoardRig : rig::Rig {
    tesserkit::System system{dev};
    tesserkit::Network net{dev};
    Board board{dev, &net};
    BoardRig() {
        board.led(kLed).button(kButton);
        fake.pins.level[kButton] = true;  // released: pulled up
    }
    void air() {
        fake::Air a;
        a.ssid = "Home";
        a.password = "password1";
        a.channel = 6;
        fake.wifi.air.push_back(a);
    }
    // Online on "Home".
    void online() {
        air();
        net.defaultNetwork("Home", "password1");
        CHECK(dev.begin());
        settle();
        CHECK(net.connected());
    }
    // The button held `ms`, then released (and debounced).
    void hold(uint32_t ms) {
        fake.pins.level[kButton] = false;
        run(ms);
        fake.pins.level[kButton] = true;
        run(100);
    }
    bool led() { return fake.pins.outputs[kLed]; }
    // The LED's levels sampled every 20 ms over `ms`: how many times it lit.
    int flashes(uint32_t ms) {
        int n = 0;
        bool was = led();
        for (uint32_t t = 0; t < ms; t += 20) {
            step(20);
            if (led() && !was) n++;
            was = led();
        }
        return n;
    }
    std::string pattern() { return value("/sys/board/led/pattern"); }
    int restarts() { return fake.core.restarts; }
};

TEST(board_tree_and_pins) {
    BoardRig r;
    r.online();
    CHECK_EQ(r.value("/sys/board/led/status"), "\"on GPIO 2\"");
    CHECK_EQ(r.value("/sys/board/button/status"), "\"on GPIO 17\"");
    CHECK(r.fake.pins.pulled[kButton] == tesserkit::Pins::Pull::Up);
    CHECK_EQ(r.value("/sys/board/button/short"), "\"accessPoint\"");
    CHECK_EQ(r.value("/sys/board/button/long"), "\"restart\"");
    CHECK_EQ(r.value("/sys/board/button/veryLong"), "\"factoryReset\"");
    CHECK_EQ(r.set("/sys/board/led/gpio", "49").status, Status::InvalidValue);
    // Pins are applied at the next restart: saved now.
    CHECK_EQ(r.set("/sys/board/led/gpio", "4").status, Status::Ok);
    CHECK_EQ(r.value("/sys/board/led/status"), "\"on GPIO 2\"");
}

TEST(board_led_follows_the_connection) {
    BoardRig r;
    r.net.defaultNetwork("Home", "password1");  // not in range yet
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.dev.state(), State::Connecting);
    CHECK_EQ(r.pattern(), "\"connecting\"");
    int n = r.flashes(3000);  // 1 Hz
    CHECK(n >= 2 && n <= 4);
    r.air();
    r.run(35000);
    CHECK(r.net.connected());
    CHECK_EQ(r.pattern(), "\"online\"");
    CHECK(r.led());
    CHECK_EQ(r.flashes(3000), 0);
    CHECK(r.led());  // steady
    // Dark while all is well.
    CHECK_EQ(r.set("/sys/board/led/show", "\"problems\"").status, Status::Ok);
    r.step();
    CHECK_EQ(r.pattern(), "\"off\"");
    CHECK(!r.led());
    r.fake.wifi.air.clear();  // the router gone
    r.fake.wifi.drop();
    r.run(500);
    CHECK_EQ(r.pattern(), "\"connecting\"");
}

TEST(board_led_active_low_and_updating) {
    BoardRig r;
    r.board.led(kLed, true);
    r.online();
    CHECK_EQ(r.pattern(), "\"online\"");
    CHECK(!r.led());  // lit: driven low
    r.dev.updating(true);
    r.run(100);
    CHECK_EQ(r.pattern(), "\"updating\"");
    int n = r.flashes(1000);  // 5 Hz
    CHECK(n >= 4 && n <= 6);
    r.dev.updating(false);
    r.dev.fail(true);
    r.run(100);
    CHECK_EQ(r.pattern(), "\"error\"");
}

TEST(board_fresh_device_shows_its_access_point) {
    BoardRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.fake.wifi.apUp);  // recovery
    CHECK_EQ(r.pattern(), "\"accessPoint\"");
    CHECK_EQ(r.flashes(3000), 6);  // a double blink every second
}

TEST(board_short_press_opens_the_access_point) {
    BoardRig r;
    r.online();
    r.run(tesserkit::Network::kApLingerMs);
    CHECK(!r.fake.wifi.apUp);
    r.hold(200);
    CHECK(r.fake.wifi.apUp);
    CHECK_EQ(r.value("/sys/network/ap/status/override"), "\"open\"");
    CHECK_EQ(r.pattern(), "\"accessPoint\"");  // although online
    CHECK(r.net.connected());
    // Ten minutes, then it follows its mode again: down, the device online.
    r.run(Board::kApOverrideMs - 1000);
    CHECK(r.fake.wifi.apUp);
    r.run(2000);
    CHECK(!r.fake.wifi.apUp);
    CHECK_EQ(r.value("/sys/network/ap/status/override"), "\"none\"");
    CHECK_EQ(r.pattern(), "\"online\"");

    // Pressed again before then, it closes.
    r.hold(200);
    CHECK(r.fake.wifi.apUp);
    r.hold(200);
    CHECK(!r.fake.wifi.apUp);
    CHECK_EQ(r.value("/sys/network/ap/status/override"), "\"closed\"");
    CHECK_EQ(r.restarts(), 0);
}

TEST(board_opened_access_point_stays_while_a_station_is_on_it) {
    BoardRig r;
    r.online();
    r.run(tesserkit::Network::kApLingerMs);
    r.hold(200);
    CHECK(r.fake.wifi.apUp);
    r.fake.wifi.apStations(1);
    r.run(Board::kApOverrideMs + 5000);
    CHECK(r.fake.wifi.apUp);
    r.fake.wifi.apStations(0);
    r.run(tesserkit::Network::kPollMs + 100);
    CHECK(!r.fake.wifi.apUp);
}

TEST(board_short_press_closes_a_recovery_access_point_for_a_while) {
    BoardRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.fake.wifi.apUp);
    r.hold(200);
    CHECK(!r.fake.wifi.apUp);
    CHECK_EQ(r.pattern(), "\"connecting\"");
    r.run(Board::kApOverrideMs + 1000);
    CHECK(r.fake.wifi.apUp);  // recovery again
}

TEST(board_long_press_restarts_on_release) {
    BoardRig r;
    r.online();
    r.fake.pins.level[kButton] = false;
    r.run(1000);
    CHECK_EQ(r.pattern(), "\"pressed\"");
    CHECK(!r.led());  // dark while down
    r.run(2500);
    CHECK_EQ(r.pattern(), "\"held\"");  // letting go now restarts
    CHECK(r.led());
    CHECK_EQ(r.restarts(), 0);
    r.fake.pins.level[kButton] = true;
    r.run(1000);
    CHECK_EQ(r.restarts(), 1);
    CHECK(!r.fake.wifi.apUp);  // not the short press's action too
}

TEST(board_very_long_press_resets_on_release) {
    BoardRig r;
    r.online();
    r.run(2500);  // the settings saved
    CHECK(!r.storage.records.empty());
    r.fake.pins.level[kButton] = false;
    r.run(11000);
    CHECK_EQ(r.pattern(), "\"factoryReset\"");
    CHECK_EQ(r.restarts(), 0);  // not the long press's restart on the way
    CHECK(!r.storage.records.empty());
    r.fake.pins.level[kButton] = true;
    r.run(1000);  // erased at the next loop, restarted 500 ms later
    CHECK(r.storage.records.empty());
    CHECK_EQ(r.restarts(), 1);
}

TEST(board_no_action_shows_nothing) {
    BoardRig r;
    r.board.actions(Board::Action::AccessPoint, Board::Action::Restart, Board::Action::None);
    r.online();
    r.fake.pins.level[kButton] = false;
    r.run(11000);
    CHECK_EQ(r.pattern(), "\"online\"");  // letting go now does nothing
    r.fake.pins.level[kButton] = true;
    r.run(1000);
    CHECK_EQ(r.restarts(), 0);
    CHECK(!r.storage.records.empty());
}

TEST(board_application_presses) {
    BoardRig r;
    std::vector<tesserkit::Press> presses;
    r.board.actions(Board::Action::Application, Board::Action::Application, Board::Action::None)
        .onPress([&presses](tesserkit::Press p) { presses.push_back(p); });
    r.online();
    r.hold(200);
    r.hold(4000);
    CHECK(presses == std::vector<tesserkit::Press>({tesserkit::Press::Short, tesserkit::Press::Long}));
    CHECK_EQ(r.restarts(), 0);
}

TEST(board_press_and_identify_actions) {
    BoardRig r;
    r.online();
    CHECK_EQ(r.set("/sys/board/button/press", "\"twice\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/board/button/press", "\"short\"").status, Status::Ok);
    r.step();
    CHECK(r.fake.wifi.apUp);
    CHECK_EQ(r.set("/sys/board/identify").status, Status::Ok);
    r.step();
    CHECK_EQ(r.pattern(), "\"identify\"");
    r.run(Board::kIdentifyMs);
    CHECK_EQ(r.pattern(), "\"accessPoint\"");
    // Shown even when the LED is off otherwise.
    CHECK_EQ(r.set("/sys/board/led/show", "\"off\"").status, Status::Ok);
    r.step();
    CHECK_EQ(r.pattern(), "\"off\"");
    r.set("/sys/board/identify");
    r.step();
    CHECK_EQ(r.pattern(), "\"identify\"");
}

TEST(board_refuses_pins_in_use) {
    BoardRig r;
    r.net.ethernet(tesserkit::boards::gledopto());
    r.board.led(0).button(kButton);  // GPIO 0: the Ethernet clock
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.value("/sys/board/led/status"), "\"GPIO 0 is Ethernet's\"");
    CHECK_EQ(r.fake.pins.outputs.count(0), 0u);
    CHECK_EQ(r.value("/sys/board/button/status"), "\"on GPIO 17\"");

    BoardRig same;
    same.board.led(kButton);
    CHECK(same.dev.begin());
    CHECK_EQ(same.value("/sys/board/led/status"), "\"GPIO 17 is the button's\"");
    CHECK_EQ(same.value("/sys/board/button/status"), "\"GPIO 17 is the LED's\"");

    BoardRig flash;
    flash.board.led(6);
    flash.fake.pins.notOutputs.insert(6);
    CHECK(flash.dev.begin());
    CHECK_EQ(flash.value("/sys/board/led/status"), "\"GPIO 6 is in use or can't be an output\"");
}

TEST(board_none_by_default) {
    rig::Rig r;
    tesserkit::Board board(r.dev);
    CHECK(r.dev.begin());
    r.run(100);
    CHECK_EQ(r.value("/sys/board/led/status"), "\"no LED\"");
    CHECK_EQ(r.value("/sys/board/button/status"), "\"no button\"");
    CHECK(r.fake.pins.outputs.empty());
    CHECK_EQ(r.value("/sys/board/led/pattern"), "\"online\"");  // no Network: Device::state()
}

TEST(ethernet_board_uses_its_pins) {
    tesserkit::EthernetBoard g = tesserkit::boards::gledopto();
    for (int8_t p : {0, 5, 19, 21, 22, 23, 25, 26, 27, 33}) CHECK(g.uses(p));
    for (int8_t p : {-1, 2, 4, 12, 13, 16, 17, 18}) CHECK(!g.uses(p));
    tesserkit::EthernetBoard w = tesserkit::boards::w5500(7, 8, 9, 44, 43);
    CHECK(w.uses(7) && w.uses(44) && w.uses(43));
    CHECK(!w.uses(19) && !w.uses(0) && !w.uses(-1));
}

}  // namespace board_cases
