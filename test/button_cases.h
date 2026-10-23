// Button (docs/DESIGN.md section 5.23): debounced short, long and very long
// presses. From pixled-controller's cases.
#pragma once

#include <vector>

#include "rig.h"

namespace button_cases {

using tesserkit::Button;
using tesserkit::Press;

// The button held `down` from `fromMs` to `toMs`, sampled every 20 ms as the
// device's task does: the presses told.
inline std::vector<Press> hold(Button& b, bool down, uint32_t fromMs, uint32_t toMs) {
    std::vector<Press> presses;
    for (uint32_t t = fromMs; t < toMs; t += 20) {
        Press p = b.update(down, t);
        if (p != Press::None) presses.push_back(p);
    }
    return presses;
}

TEST(button_short_press) {
    Button b;
    CHECK(hold(b, true, 0, 300).empty());
    CHECK(b.pressed());
    CHECK_EQ(b.heldMs(300), 300u);
    std::vector<Press> p = hold(b, false, 300, 400);
    CHECK(p == std::vector<Press>({Press::Short}));
    CHECK(!b.pressed());
    CHECK_EQ(b.heldMs(400), 0u);
}

TEST(button_long_press_while_held) {
    Button b;
    CHECK(hold(b, true, 0, 1100) == std::vector<Press>({Press::Long}));
    // Held on, then released: nothing more (no very long press asked).
    CHECK(hold(b, true, 1100, 12000).empty());
    CHECK(hold(b, false, 12000, 12200).empty());
    // The next press is new.
    hold(b, true, 12200, 12400);
    CHECK(hold(b, false, 12400, 12500) == std::vector<Press>({Press::Short}));
}

TEST(button_very_long_press) {
    Button::Options o;
    o.longMs = 500;
    o.veryLongMs = 10000;
    Button b(o);
    CHECK(hold(b, true, 0, 600) == std::vector<Press>({Press::Long}));
    CHECK(hold(b, true, 600, 9980).empty());
    CHECK(hold(b, true, 9980, 10100) == std::vector<Press>({Press::VeryLong}));
    CHECK(hold(b, true, 10100, 20000).empty());
    CHECK(hold(b, false, 20000, 20200).empty());
}

TEST(button_bounces) {
    // Contact bounce under the debounce time is no press, and doesn't split
    // one.
    Button b;
    CHECK(b.update(true, 0) == Press::None);
    CHECK(b.update(false, 10) == Press::None);
    CHECK(b.update(false, 100) == Press::None);
    CHECK(!b.pressed());
    b.update(true, 200);
    b.update(true, 240);
    CHECK(b.pressed());
    b.update(false, 260);
    b.update(true, 270);
    CHECK(b.update(true, 400) == Press::None);
    CHECK(b.pressed());
}

TEST(button_on_a_pin) {
    rig::Rig r;
    Button b;
    std::vector<Press> presses;
    r.fake.pins.level[17] = true;  // released: pulled up
    CHECK(b.attach(r.dev, 17, true, [&](Press p) { presses.push_back(p); }));
    CHECK(r.fake.pins.pulled[17] == tesserkit::Pins::Pull::Up);
    Button high;  // active high: pulled down
    r.fake.pins.level[4] = false;
    CHECK(high.attach(r.dev, 4, false, [](Press) {}));
    CHECK(r.fake.pins.pulled[4] == tesserkit::Pins::Pull::Down);
    CHECK(!high.attach(r.dev, 40, false, [](Press) {}));  // the fake has no such input
    CHECK(r.dev.begin());
    r.run(200);
    CHECK(presses.empty());
    r.fake.pins.level[17] = false;  // pressed
    r.run(200);
    r.fake.pins.level[17] = true;
    r.run(100);
    CHECK(presses == std::vector<Press>({Press::Short}));
    r.fake.pins.level[17] = false;
    r.run(1200);
    CHECK(presses == std::vector<Press>({Press::Short, Press::Long}));
}

}  // namespace button_cases
