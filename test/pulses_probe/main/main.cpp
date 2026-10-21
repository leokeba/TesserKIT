// A test firmware for PulseOutput (docs/DESIGN.md section 5.21): pulse trains
// on PULSES_GPIO, set over HTTP under /probe, and the pin's own edges timed
// by an MCPWM capture on the same pin (80 MHz on the classic ESP32): the
// jitter of what the pin does, not of what the interrupt thinks it did.
//
// With one train and no cap, /probe/capture says how far each pulse's start
// was from one period after the previous one's, and each pulse's length from
// the one asked. Load it meanwhile: HTTP requests, settings saved (NVS), a
// firmware upload (flash erased and written), the Wi-Fi.
//
// Wi-Fi: a gitignored main/secrets.h may define WIFI_SSID and WIFI_PASSWORD;
// else save a network over the serial line or the access point.
#include <math.h>

#include "TesserKIT.h"
#include "driver/mcpwm_cap.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#if !defined(PULSES_GPIO)
#define PULSES_GPIO 4
#endif

namespace {

tesserkit::Device dev("Pulses probe");
tesserkit::System sys(dev);
tesserkit::Network net(dev);
tesserkit::Firmware firmware(dev);
tesser::UartTransport serialApi(dev.api(), UART_NUM_0);
tesserkit::PulseOutput out(tesserkit::espPulses(), PULSES_GPIO);

// The capture's edges, from its interrupt to the task.
struct Edge {
    uint32_t ticks;
    bool rising;
};
constexpr uint32_t kRing = 2048;
DRAM_ATTR Edge g_ring[kRing];
volatile uint32_t g_head = 0;
volatile uint32_t g_tail = 0;
volatile uint32_t g_overruns = 0;
uint32_t g_tickHz = 0;

bool IRAM_ATTR onCapture(mcpwm_cap_channel_handle_t, const mcpwm_capture_event_data_t* e, void*) {
    uint32_t h = g_head;
    if (h - g_tail >= kRing) {
        g_overruns = g_overruns + 1;
        return false;
    }
    g_ring[h % kRing] = {e->cap_value, e->cap_edge == MCPWM_CAP_EDGE_POS};
    g_head = h + 1;
    return false;
}

struct Probe {
    // The trains: `trains` of them, the i-th at hz + i · stepHz, its phase
    // i · phaseStepUs.
    bool on = true;
    int32_t trains = 1;
    double hz = 60;
    double stepHz = 0;
    double pulseUs = 500;
    double phaseStepUs = 0;
    double offsetUs = 0;
    double rate = 1;
    double capUs = 0;
    // The engine's counts.
    uint32_t alarms = 0, edges = 0, late = 0, worstLateUs = 0, capped = 0;
    // The capture's, with one train.
    uint32_t pulses = 0, overruns = 0;
    double periodErrMaxUs = 0, periodErrMeanUs = 0, lengthErrMaxUs = 0;
    double periodErrSum = 0;
    uint32_t periodSamples = 0;
    // The capture's state.
    bool haveRise = false;
    uint32_t lastRise = 0;
} probe;

void apply() {
    for (int i = 0; i < tesserkit::PulseOutput::kTrains; i++) {
        tesserkit::pulses::Train t;
        t.on = probe.on && i < probe.trains;
        t.periodUs = 1e6 / (probe.hz + i * probe.stepHz);
        t.pulseUs = probe.pulseUs;
        t.phaseUs = i * probe.phaseStepUs;
        out.setTrain(i, t);
    }
    tesserkit::pulses::Timebase tb;
    tb.offsetUs = probe.offsetUs;
    tb.rate = probe.rate;
    out.setTimebase(tb);
    out.setMaxPulseUs(probe.capUs);
}

void resetCounts() {
    out.resetStats();
    probe.pulses = 0;
    probe.periodErrMaxUs = probe.periodErrMeanUs = probe.lengthErrMaxUs = probe.periodErrSum = 0;
    probe.periodSamples = 0;
    probe.haveRise = false;
    g_overruns = 0;
}

// The capture's edges: one train's periods and lengths.
void drain() {
    double period = 1e6 / probe.hz / probe.rate;
    bool one = probe.trains == 1 && probe.capUs == 0;
    while (g_tail != g_head) {
        Edge e = g_ring[g_tail % kRing];
        g_tail = g_tail + 1;
        if (e.rising) {
            probe.pulses++;
            if (one && probe.haveRise) {
                double dt = static_cast<double>(e.ticks - probe.lastRise) * 1e6 / g_tickHz;
                double err = fabs(dt - period);
                if (err < period / 2) {  // a missed edge isn't jitter
                    probe.periodErrSum += err;
                    probe.periodSamples++;
                    if (err > probe.periodErrMaxUs) probe.periodErrMaxUs = err;
                }
            }
            probe.lastRise = e.ticks;
            probe.haveRise = true;
        } else if (one && probe.haveRise) {
            double len = static_cast<double>(e.ticks - probe.lastRise) * 1e6 / g_tickHz;
            double err = fabs(len - probe.pulseUs / probe.rate);
            if (err > probe.lengthErrMaxUs) probe.lengthErrMaxUs = err;
        }
    }
    probe.periodErrMeanUs = probe.periodSamples ? probe.periodErrSum / probe.periodSamples : 0;
    probe.overruns = g_overruns;
    tesserkit::pulses::Stats s = out.stats();
    probe.alarms = s.alarms;
    probe.edges = s.edges;
    probe.late = s.late;
    probe.worstLateUs = s.worstLateUs;
    probe.capped = s.capped;
}

// A setting's setter: stored, then applied.
template <typename T>
std::function<void(T)> setter(T& field) {
    return [&field](T v) {
        field = v;
        apply();
    };
}

void describe(tesser::Object& o, Probe&) {
    o.value("on", [] { return probe.on; }, setter(probe.on)).persist();
    o.value("trains", [] { return probe.trains; }, setter(probe.trains)).range(0, 8).persist();
    o.value("hz", [] { return probe.hz; }, setter(probe.hz)).range(0.01, 20000).persist();
    o.value("stepHz", [] { return probe.stepHz; }, setter(probe.stepHz)).persist();
    o.value("pulseUs", [] { return probe.pulseUs; }, setter(probe.pulseUs)).range(0, 1e6).persist();
    o.value("phaseStepUs", [] { return probe.phaseStepUs; }, setter(probe.phaseStepUs)).persist();
    o.value("offsetUs", [] { return probe.offsetUs; }, setter(probe.offsetUs));
    o.value("rate", [] { return probe.rate; }, setter(probe.rate));
    o.value("capUs", [] { return probe.capUs; }, setter(probe.capUs)).persist();
    auto& s = o.object("stats");
    s.value("alarms", static_cast<const uint32_t&>(probe.alarms));
    s.value("edges", static_cast<const uint32_t&>(probe.edges));
    s.value("late", static_cast<const uint32_t&>(probe.late));
    s.value("worstLateUs", static_cast<const uint32_t&>(probe.worstLateUs));
    s.value("capped", static_cast<const uint32_t&>(probe.capped));
    auto& c = o.object("capture");
    c.value("pulses", static_cast<const uint32_t&>(probe.pulses));
    c.value("periodErrMaxUs", static_cast<const double&>(probe.periodErrMaxUs));
    c.value("periodErrMeanUs", static_cast<const double&>(probe.periodErrMeanUs));
    c.value("lengthErrMaxUs", static_cast<const double&>(probe.lengthErrMaxUs));
    c.value("overruns", static_cast<const uint32_t&>(probe.overruns));
    o.action("reset", [] { resetCounts(); });
    // A save: the settings written to NVS now, as a setter's would be.
    o.value("scratch", [] { return 0; }, [](int32_t) {}).persist();
}

bool startCapture() {
    mcpwm_capture_timer_config_t tc = {};
    tc.group_id = 0;
    tc.clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT;
    mcpwm_cap_timer_handle_t timer = nullptr;
    if (mcpwm_new_capture_timer(&tc, &timer) != ESP_OK) return false;
    mcpwm_capture_channel_config_t cc = {};
    cc.gpio_num = PULSES_GPIO;
    cc.prescale = 1;
    cc.flags.pos_edge = true;
    cc.flags.neg_edge = true;
    mcpwm_cap_channel_handle_t channel = nullptr;
    if (mcpwm_new_capture_channel(timer, &cc, &channel) != ESP_OK) return false;
    mcpwm_capture_event_callbacks_t cb = {};
    cb.on_cap = onCapture;
    if (mcpwm_capture_channel_register_event_callbacks(channel, &cb, nullptr) != ESP_OK) return false;
    mcpwm_capture_channel_enable(channel);
    mcpwm_capture_timer_enable(timer);
    mcpwm_capture_timer_start(timer);
    mcpwm_capture_timer_get_resolution(timer, &g_tickHz);
    return g_tickHz > 0;
}

}  // namespace

extern "C" void app_main() {
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
    net.defaultNetwork(WIFI_SSID, WIFI_PASSWORD);
#endif
    net.apMode(tesserkit::ApMode::Fallback);
    dev.api().mount("probe", probe).label("Pulses");
    dev.onTick([](uint32_t) {
        out.poll();
        drain();
    });
    dev.cors("https://leokeba.github.io");
    dev.begin();
    serialApi.begin();
    {
        tesser::Lock lock(dev.api());
        apply();
        if (!out.begin()) tesser::logError("probe: PulseOutput didn't start");
    }
    // The capture's interrupt on core 1 too: on core 0, Wi-Fi holds it long
    // enough for an edge's capture to be overwritten by the next.
    xTaskCreatePinnedToCore(
        [](void*) {
            if (!startCapture()) tesser::logError("probe: no capture");
            vTaskDelete(nullptr);
        },
        "probe_cap", 4096, nullptr, 5, nullptr, 1);
}
