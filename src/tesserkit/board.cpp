#include "tesserkit/board.h"

#include "tesserkit/network.h"

namespace tesserkit {

using tesser::Access;
using tesser::Status;

Board& Board::led(int8_t pin, bool activeLow) {
    ledGpio_ = pin;
    ledActiveLow_ = activeLow;
    return *this;
}

Board& Board::button(int8_t pin, bool activeLow) {
    buttonGpio_ = pin;
    buttonActiveLow_ = activeLow;
    return *this;
}

Board& Board::actions(Action shortPress, Action longPress, Action veryLongPress) {
    actions_[0] = shortPress;
    actions_[1] = longPress;
    actions_[2] = veryLongPress;
    return *this;
}

Board& Board::onPress(std::function<void(Press)> fn) {
    onPress_ = std::move(fn);
    return *this;
}

void Board::describe(tesser::Object& o) {
    Platform& platform = dev_.platform();
    if (!platform.pins) platform.pins = espDigitalPins();
    pins_ = platform.pins;

    o.label("LED and button").ui("icon", "power").writeAccess(Access::Admin);
    auto& l = o.object("led").label("Status LED").ui("card");
    ledNode_ = &l;
    l.value("gpio", ledGpio_).range(-1, 48).persist().label("GPIO").doc("-1: none. Applied at the next restart");
    l.value("activeLow", ledActiveLow_).persist().label("Lit when low").ui("advanced");
    l.value("show", show_, {"status", "problems", "off"})
        .persist()
        .ui("labels", "Always,Only when not online,Off");
    l.value("status", static_cast<const std::string&>(ledStatus_));
    patternNode_ = &l.value("pattern", [this] { return pattern_; },
                            {"off", "online", "connecting", "accessPoint", "updating", "error", "identify",
                             "pressed", "held", "factoryReset"})
                        .label("Showing");
    l.action("identify", [this] { identify(); }).doc("Blinks the LED for 10 s, to find the board");

    auto& b = o.object("button").label("Button").ui("card");
    buttonNode_ = &b;
    b.value("gpio", buttonGpio_).range(-1, 48).persist().label("GPIO").doc("-1: none. Applied at the next restart");
    b.value("activeLow", buttonActiveLow_).persist().label("Pressed when low").ui("advanced");
    static const char* const kLabels = "Nothing,Open or close the access point,Restart,Factory reset,The application's";
    const std::initializer_list<const char*> names = {"none", "accessPoint", "restart", "factoryReset", "application"};
    b.value("short", actions_[0], names).persist().label("Short press").ui("labels", kLabels);
    b.value("long", actions_[1], names).persist().label("Held 3 s").ui("labels", kLabels);
    b.value("veryLong", actions_[2], names).persist().label("Held 10 s").ui("labels", kLabels);
    b.value("status", static_cast<const std::string&>(buttonStatus_));
    b.action("press",
             [this](const std::string& which) {
                 Press p = which == "short"      ? Press::Short
                           : which == "long"     ? Press::Long
                           : which == "veryLong" ? Press::VeryLong
                                                 : Press::None;
                 if (p == Press::None) return Status::InvalidValue;
                 press(p);
                 return Status::Ok;
             })
        .doc("Does a press's action, as if the button was pressed: short, long or veryLong")
        .ui("advanced");
    pressed_ = &b.event("pressed").label("Pressed").ui("toast", "info").ui("hidden");

    dev_.onSleep([this] {
        if (ledOk_) pins_->output(ledGpio_, ledActiveLow_);  // dark while asleep
    });
}

std::string Board::refusal(int8_t pin, int8_t other, const char* otherName) const {
    std::string gpio = "GPIO " + std::to_string(pin);
    const EthernetBoard* eth = net_ ? net_->ethernetBoard() : nullptr;
    if (eth && eth->uses(pin)) return gpio + " is Ethernet's";
    if (pin == other) return gpio + " is the " + otherName + "'s";
    return "";
}

void Board::start() {
    if (ledGpio_ >= 0) {
        std::string why = refusal(ledGpio_, buttonGpio_, "button");
        if (why.empty() && !(pins_ && pins_->outputBegin(ledGpio_))) {
            why = "GPIO " + std::to_string(ledGpio_) + " is in use or can't be an output";
        }
        ledOk_ = why.empty();
        ledStatus_ = ledOk_ ? "on GPIO " + std::to_string(ledGpio_) : why;
        if (ledOk_) pins_->output(ledGpio_, ledActiveLow_);
        else tesser::logError("tesserkit: Board: status LED: %s", why.c_str());
    }
    if (buttonGpio_ >= 0) {
        std::string why = refusal(buttonGpio_, ledGpio_, "LED");
        Pins::Pull pull = buttonActiveLow_ ? Pins::Pull::Up : Pins::Pull::Down;
        if (why.empty() && !(pins_ && pins_->inputBegin(buttonGpio_, pull))) {
            why = "GPIO " + std::to_string(buttonGpio_) + " can't be an input";
        }
        buttonOk_ = why.empty();
        buttonStatus_ = buttonOk_ ? "on GPIO " + std::to_string(buttonGpio_) : why;
        if (!buttonOk_) tesser::logError("tesserkit: Board: button: %s", why.c_str());
    }
    ledNode_->changed();
    buttonNode_->changed();
}

void Board::tick(uint32_t nowMs) {
    if (buttonOk_) {
        Press p = button_.update(pins_->input(buttonGpio_) != buttonActiveLow_, nowMs);
        // Long presses are told while held: their actions wait for the
        // release, so holding on to the very long time doesn't restart first.
        if (p == Press::Short) {
            press(p);
        } else if (p != Press::None) {
            armed_ = p;
        }
        if (down_ && !button_.pressed() && armed_ != Press::None) {
            Press done = armed_;
            armed_ = Press::None;
            press(done);
        }
        down_ = button_.pressed();
    }
    if (identifying_ && static_cast<int32_t>(nowMs - identifyUntilMs_) >= 0) identifying_ = false;
    Pattern pat = choose();
    if (pat != pattern_) {
        pattern_ = pat;
        patternNode_->changed();
    }
    bool on = lit(pat, nowMs);
    if (ledOk_ && on != lit_) pins_->output(ledGpio_, on != ledActiveLow_);
    lit_ = on;
}

void Board::identify(uint32_t ms) {
    identifying_ = true;
    identifyUntilMs_ = dev_.loopMs() + ms;
}

Board::Action Board::actionOf(Press p) const {
    switch (p) {
        case Press::Short: return actions_[0];
        case Press::Long: return actions_[1];
        case Press::VeryLong: return actions_[2];
        case Press::None: break;
    }
    return Action::None;
}

void Board::press(Press p) { run(actionOf(p), p); }

void Board::toast(const char* message) { pressed_->emit(message); }

void Board::run(Action a, Press p) {
    switch (a) {
        case Action::None: return;
        case Action::AccessPoint: {
            if (!net_) {
                tesser::logWarning("tesserkit: Board: no Network for the access point");
                return;
            }
            bool open = !net_->apActive();
            net_->overrideAp(open ? ApOverride::Open : ApOverride::Closed, kApOverrideMs);
            toast(open ? "Access point open for 10 minutes" : "Access point closed for 10 minutes");
            return;
        }
        case Action::Restart:
            toast("Restarting");
            dev_.restartIn(500);
            return;
        case Action::FactoryReset:
            toast("Factory reset");
            dev_.factoryReset();
            return;
        case Action::Application:
            if (onPress_) onPress_(p);
            return;
    }
}

Board::Pattern Board::choose() const {
    // The button first: what letting go now does.
    if (buttonOk_ && button_.pressed()) {
        if (armed_ == Press::None) return Pattern::Pressed;
        Action a = actionOf(armed_);
        if (a == Action::FactoryReset) return Pattern::FactoryReset;
        if (a != Action::None) return Pattern::Held;
    }
    if (identifying_) return Pattern::Identify;
    if (show_ == Show::Off) return Pattern::Off;
    State s = dev_.state();
    if (s == State::Updating) return Pattern::Updating;
    if (s == State::Error) return Pattern::Error;
    bool online = s == State::Online;
    if (net_ && net_->apActive() && (!online || net_->apOverride() == ApOverride::Open)) return Pattern::AccessPoint;
    if (online) return show_ == Show::Problems ? Pattern::Off : Pattern::Online;
    return Pattern::Connecting;
}

bool Board::lit(Pattern p, uint32_t nowMs) {
    switch (p) {
        case Pattern::Off:
        case Pattern::Pressed: return false;
        case Pattern::Online:
        case Pattern::Held: return true;
        case Pattern::Connecting: return nowMs % 1000 < 500;
        case Pattern::AccessPoint: {
            uint32_t t = nowMs % 1000;
            return t < 100 || (t >= 200 && t < 300);
        }
        case Pattern::Updating:
        case Pattern::FactoryReset: return nowMs % 200 < 100;
        case Pattern::Error: {
            uint32_t t = nowMs % 2000;
            return t < 600 && t % 200 < 100;
        }
        case Pattern::Identify: return nowMs % 400 < 200;
    }
    return false;
}

}  // namespace tesserkit
