// The log's output: esp_log's vprintf, hooked. Lines still go to the
// console; a copy waits in a ring for the device's task.
#if defined(ESP_PLATFORM)

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "tesserkit/log_ring.h"
#include "tesserkit/platform.h"

namespace tesserkit {

namespace {

constexpr size_t kLineMax = 160;  // longer lines are cut: this runs on every task's stack

class EspLog : public LogOutput {
public:
    bool begin(size_t bytes) override {
        if (buf_) return true;
        char* buf = static_cast<char*>(malloc(bytes));
        if (!buf) return false;
        portENTER_CRITICAL(&mux_);
        ring_.attach(buf, bytes);
        buf_ = buf;
        portEXIT_CRITICAL(&mux_);
        previous_ = esp_log_set_vprintf(&EspLog::hook);
        return true;
    }
    uint8_t level() override { return static_cast<uint8_t>(esp_log_level_get("*")); }
    void level(uint8_t l) override { esp_log_level_set("*", static_cast<esp_log_level_t>(l)); }
    uint8_t maxLevel() override { return CONFIG_LOG_MAXIMUM_LEVEL; }
    bool next(std::string& line) override {
        char buf[kLineMax];
        size_t n = 0;
        portENTER_CRITICAL(&mux_);
        bool any = ring_.next(buf, sizeof(buf), n);
        portEXIT_CRITICAL(&mux_);
        if (any) line.assign(buf, n);  // out of the critical section: it may allocate
        return any;
    }
    size_t recentSize() override {
        portENTER_CRITICAL(&mux_);
        size_t n = ring_.size();
        portEXIT_CRITICAL(&mux_);
        return n;
    }
    size_t recent(char* out, size_t cap) override {
        // Two memcpys of up to the ring's size (4 KB by default): a few µs
        // with interrupts off on this core.
        portENTER_CRITICAL(&mux_);
        size_t n = ring_.copy(out, cap);
        portEXIT_CRITICAL(&mux_);
        return n;
    }
    uint32_t dropped() override { return dropped_; }
    void mute(bool on) override { muted_ = on ? xTaskGetCurrentTaskHandle() : nullptr; }

    static EspLog instance;

private:
    static int hook(const char* fmt, va_list ap) {
        EspLog& self = instance;
        va_list copy;
        va_copy(copy, ap);
        int n = self.previous_ ? self.previous_(fmt, ap) : vprintf(fmt, ap);
        if (!xPortInIsrContext() && xTaskGetCurrentTaskHandle() != self.muted_) {
            char line[kLineMax];
            vsnprintf(line, sizeof(line), fmt, copy);
            self.capture(line);
        }
        va_end(copy);
        return n;
    }

    // Appends what was printed, without colours, to the line being
    // assembled (ESP-IDF's log v2 prints a line in several calls), and
    // keeps the lines it completes.
    void capture(char* text) {
        size_t w = 0;
        for (size_t r = 0; text[r]; r++) {
            if (text[r] == '\033') {  // ESC [ ... m
                while (text[r] && text[r] != 'm') r++;
                if (!text[r]) break;
                continue;
            }
            if (text[r] != '\r') text[w++] = text[r];
        }
        portENTER_CRITICAL(&mux_);
        for (size_t i = 0; i < w; i++) {
            if (text[i] == '\n') {
                keep();
            } else if (partialLen_ < sizeof(partial_)) {
                partial_[partialLen_++] = text[i];
            }
        }
        portEXIT_CRITICAL(&mux_);
    }

    // The assembled line into the ring, dropping the oldest lines the Log
    // service has taken already. In the critical section.
    void keep() {
        size_t n = partialLen_;
        partialLen_ = 0;
        if (n && !ring_.push(partial_, n)) dropped_ = dropped_ + 1;
    }

    vprintf_like_t previous_ = nullptr;
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    char* buf_ = nullptr;  // the ring's, allocated once
    LogRing ring_;
    char partial_[kLineMax];
    size_t partialLen_ = 0;
    volatile uint32_t dropped_ = 0;
    TaskHandle_t volatile muted_ = nullptr;
};

EspLog EspLog::instance;

}  // namespace

LogOutput* espLog() { return &EspLog::instance; }

}  // namespace tesserkit

#endif
