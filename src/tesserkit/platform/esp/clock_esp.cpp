// The system clock, the time zone and SNTP.
#if defined(ESP_PLATFORM)

#include "tesserkit/platform.h"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include <string>

#include "esp_sntp.h"

namespace tesserkit {

namespace {

class EspClock : public Clock {
public:
    void begin(EventSink sink) override { sink_ = std::move(sink); }
    int64_t epochMs() override {
        timeval tv;
        gettimeofday(&tv, nullptr);
        return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
    }
    void setEpochMs(int64_t ms) override {
        timeval tv;
        tv.tv_sec = static_cast<time_t>(ms / 1000);
        tv.tv_usec = static_cast<suseconds_t>(ms % 1000 * 1000);
        settimeofday(&tv, nullptr);
    }
    void timezone(const char* posix) override {
        setenv("TZ", posix, 1);
        tzset();
    }
    void sntp(bool on, const char* server) override {
        if (esp_sntp_enabled()) esp_sntp_stop();
        if (!on) return;
        server_ = server;  // SNTP keeps the pointer
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, server_.c_str());
        sntp_set_time_sync_notification_cb(&EspClock::onSync);
        esp_sntp_init();
    }

private:
    static void onSync(timeval*) {
        Event e;
        e.type = Event::Type::TimeSynced;
        if (instance().sink_) instance().sink_(std::move(e));
    }
public:
    static EspClock& instance();

private:
    EventSink sink_;
    std::string server_;
};

EspClock& EspClock::instance() {
    static EspClock clock;
    return clock;
}

}  // namespace

Clock* espClock() { return &EspClock::instance(); }

}  // namespace tesserkit

#endif
