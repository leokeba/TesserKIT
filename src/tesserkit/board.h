#pragma once

#include <functional>
#include <string>

#include "tesserkit/button.h"
#include "tesserkit/device.h"

namespace tesserkit {

class Network;

// /sys/board: the board's status LED and function button (docs/DESIGN.md
// section 5.24). The LED tells the connection (online, connecting, the
// access point up, an update), the button opens or closes the access point,
// restarts or resets the device. Both pins are settings, applied at the next
// restart; the application gives the board's as factory defaults.
class Board : public Service {
public:
    // What a press does, on release.
    enum class Action : uint8_t { None, AccessPoint, Restart, FactoryReset, Application };
    // When the LED is lit: always with the state, only when the device isn't
    // online (dark when all is well), or never (identify() and the button's
    // feedback still show).
    enum class Show : uint8_t { Status, Problems, Off };
    // What the LED shows now.
    enum class Pattern : uint8_t {
        Off,
        Online,       // on
        Connecting,   // 1 Hz
        AccessPoint,  // a double blink every second: up while not online, or opened by the button
        Updating,     // 5 Hz
        Error,        // three quick blinks every 2 s (Device::fail())
        Identify,     // 2.5 Hz, for identify()'s time
        Pressed,      // dark while the button is down, before the long time
        Held,         // on: letting go now does the long press's action
        FactoryReset  // 5 Hz: letting go now resets the device
    };

    // Held this long, a press is long, then very long.
    static constexpr uint32_t kLongMs = 3000;
    static constexpr uint32_t kVeryLongMs = 10000;
    // How long the button keeps the access point opened or closed.
    static constexpr uint32_t kApOverrideMs = 10 * 60 * 1000;
    static constexpr uint32_t kIdentifyMs = 10000;

    // `net`: the Network the button's access point action toggles and the
    // LED follows, declared before this. Without it the LED follows
    // Device::state() and that action does nothing.
    explicit Board(Device& dev, Network* net = nullptr) : Service(dev, "board"), net_(net) {}

    // Factory defaults, before Device::begin(). The LED on `pin`, lit high
    // (or low); none by default.
    Board& led(int8_t pin, bool activeLow = false);
    // The button on `pin`, pressed low against the chip's pull-up (or high
    // against its pull-down); none by default. A Gledopto's is GPIO 17, a
    // devkit's BOOT button GPIO 0.
    Board& button(int8_t pin, bool activeLow = true);
    // The presses' actions; by default the access point, restart, factory
    // reset.
    Board& actions(Action shortPress, Action longPress, Action veryLongPress);
    // Presses whose action is Application, in the device's task.
    Board& onPress(std::function<void(Press)> fn);

    void describe(tesser::Object& o) override;

    // Blinks the LED for `ms`, to find the board. Under the API lock.
    void identify(uint32_t ms = kIdentifyMs);
    // Does what the press's action says, as if the button was pressed.
    // Under the API lock.
    void press(Press p);
    Pattern pattern() const { return pattern_; }
    // Whether `p` lights the LED at `nowMs`.
    static bool lit(Pattern p, uint32_t nowMs);

protected:
    void start() override;
    void tick(uint32_t nowMs) override;

private:
    Pattern choose() const;
    Action actionOf(Press p) const;
    void run(Action a, Press p);
    void toast(const char* message);  // the pressed event
    // Why `pin` can't be used (Ethernet's, or `other`'s), or "".
    std::string refusal(int8_t pin, int8_t other, const char* otherName) const;

    Network* net_;
    int8_t ledGpio_ = -1;
    bool ledActiveLow_ = false;
    Show show_ = Show::Status;
    int8_t buttonGpio_ = -1;
    bool buttonActiveLow_ = true;
    Action actions_[3] = {Action::AccessPoint, Action::Restart, Action::FactoryReset};
    std::function<void(Press)> onPress_;

    Pins* pins_ = nullptr;
    bool ledOk_ = false, buttonOk_ = false;
    std::string ledStatus_ = "no LED", buttonStatus_ = "no button";
    Button button_{Button::Options{30, kLongMs, kVeryLongMs}};
    Press armed_ = Press::None;  // the press a release would complete
    bool down_ = false;
    uint32_t identifyUntilMs_ = 0;
    bool identifying_ = false;
    Pattern pattern_ = Pattern::Off;
    bool lit_ = false;
    tesser::Node* patternNode_ = nullptr;
    tesser::Object* ledNode_ = nullptr;
    tesser::Object* buttonNode_ = nullptr;
    tesser::EventNode* pressed_ = nullptr;
};

}  // namespace tesserkit
