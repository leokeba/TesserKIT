// A TesserKIT device on the host, for working on its interface: every
// service on the fake platform, with Wi-Fi networks in range, a clock that
// runs, and a heap that moves. It speaks TesserUI's demo protocol on
// stdin/stdout (one "<client> <envelope>" line each way, a LineTransport per
// client), so TesserUI's web/scripts/device.mjs serves it like a node:
//
//   node ~/Dev/TesserUI/web/scripts/device.mjs --exe build/tesserkit_demo --port 8092
//   then http://localhost:8092/ (the built bundle) or
//        http://localhost:5173/?api=http://localhost:8092/api (pnpm dev)
//
// Clients are Admin, as serial lines are, unless started with --public.
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <string>

#include "TesserKIT.h"
#include "fake_platform.h"

namespace {

int64_t realMs() {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

struct Client {
    Client(tesser::Api& api, long id, bool admin)
        : prefix(std::to_string(id) + " "), line(api, [this](const char* data, size_t len) {
              out.append(data, len);
              size_t nl;
              while ((nl = out.find('\n')) != std::string::npos) {
                  fwrite(prefix.data(), 1, prefix.size(), stdout);
                  fwrite(out.data(), 1, nl + 1, stdout);
                  out.erase(0, nl + 1);
              }
              fflush(stdout);
          }) {
        line.setAuthenticated(admin);
    }
    std::string prefix;
    std::string out;
    tesser::LineTransport line;
};

struct Led {
    bool on = true;
    int brightness = 180;
    std::string color = "#ffb347";
};

void describe(tesser::Object& o, Led& l) {
    o.value("on", l.on).persist();
    o.value("brightness", l.brightness).range(0, 255).step(5).persist();
    o.value("color", l.color).ui("widget", "color").persist();
}

}  // namespace

int main(int argc, char** argv) {
    bool admin = true;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--public") == 0) admin = false;
    }

    fake::Platform fake;
    fake.core.boot.version = "1.2.0";
    fake.core.boot.project = "tesserkit-demo";
    const char* air[][3] = {{"Workshop", "workshop-pw", "-48"}, {"Freebox-24BFC2", "x", "-71"}, {"Cafe Guest", "", "-83"}};
    for (auto& a : air) {
        fake::Air n;
        n.ssid = a[0];
        n.password = a[1];
        n.rssi = static_cast<int8_t>(atoi(a[2]));
        n.channel = 6;
        fake.wifi.air.push_back(n);
    }
    fake.clock.epoch = realMs();
    fake.coredump.data = std::string(4096, 'c');

    tesser::MemoryStorage storage;
    tesserkit::Device dev("TesserKIT Demo");
    fake.install(dev);
    dev.task(false).storage(storage);
    tesserkit::System sys(dev);
    tesserkit::Network net(dev);
    tesserkit::Time timekeeper(dev);
    tesserkit::Firmware firmware(dev);
    tesserkit::Backup backup(dev);
    tesserkit::Timers timers(dev);
    tesserkit::Auth auth(dev);
    tesserkit::Mqtt mqtt(dev);
    tesserkit::ArtNet artnet(dev);
    tesserkit::Bluetooth bluetooth(dev);
    tesserkit::NowTp nowtp(dev);
    tesserkit::Sequencer sequencer(dev);
    net.defaultNetwork("Workshop", "workshop-pw");
    timekeeper.defaultTz("CET-1CEST,M3.5.0,M10.5.0/3");
    // A newer release to find and install.
    firmware.github("leokeba/TesserKIT", "tesserkit-minimal-{target}.bin");
    fake.http.files["https://api.github.com/repos/leokeba/TesserKIT/releases/latest"] =
        R"({"tag_name": "v1.3.0", "assets": [{"name": "tesserkit-minimal-esp32.bin", )"
        R"("browser_download_url": "https://example.com/tesserkit-minimal-esp32.bin"}]})";
    fake.http.files["https://example.com/tesserkit-minimal-esp32.bin"] = fake::image(0, 300000);
    tesserkit::Timer t;
    snprintf(t.name, sizeof(t.name), "%s", "morning");
    snprintf(t.time, sizeof(t.time), "%s", "07:30");
    t.days = 0x1F;
    t.path = "/led/on";
    t.value = "true";
    timers.add(t);
    // A blink: on and off every half second, until stopped (off then).
    for (bool lit : {true, false}) {
        tesserkit::Step s;
        snprintf(s.name, sizeof(s.name), "%s", lit ? "on" : "off");
        s.path = "/led/on";
        s.value = lit ? "true" : "false";
        s.wait = tesserkit::Wait::Time;
        s.ms = 500;
        s.next = lit ? "+1" : "on";
        sequencer.add(s);
    }
    sequencer.finally("/led/on", "false");
    Led led;
    dev.api().mount("led", led).label("LED").ui("icon", "bulb");
    if (!dev.begin()) return 1;
    fake.clock.sync(realMs());  // SNTP answers at once
    // Nodes nearby over ESP-NOW, once NowTP has started.
    bool nearby = false;

    std::map<long, std::unique_ptr<Client>> byId;
    std::string pending;
    auto dispatch = [&](const std::string& in) {
        char* rest = nullptr;
        long id = strtol(in.c_str(), &rest, 10);
        if (rest == in.c_str() || *rest != ' ') return;
        std::string body(rest + 1);
        if (body == "close") {
            byId.erase(id);
            return;
        }
        auto& c = byId[id];
        if (!c) c.reset(new Client(dev.api(), id, admin));
        body += '\n';
        c->line.feed(body.data(), body.size());
    };
    fprintf(stderr, "tesserkit_demo: serving on stdin/stdout\n");

    int64_t start = realMs();
    char buf[4096];
    for (;;) {
        struct pollfd pfd = {0, POLLIN, 0};
        int ready = poll(&pfd, 1, 20);
        if (ready < 0 && errno != EINTR) break;
        if (ready > 0) {
            ssize_t n = read(0, buf, sizeof(buf));
            if (n == 0) break;
            if (n < 0 && errno != EINTR && errno != EAGAIN) break;
            if (n > 0) {
                pending.append(buf, static_cast<size_t>(n));
                size_t nl;
                while ((nl = pending.find('\n')) != std::string::npos) {
                    dispatch(pending.substr(0, nl));
                    pending.erase(0, nl + 1);
                }
            }
        }
        int64_t now = realMs();
        fake.core.now = static_cast<uint64_t>(now - start) + 1000;
        fake.clock.epoch = now;
        fake.core.heap = static_cast<uint32_t>(182000 + (now / 700 % 9) * 350);
        fake.core.runTasks();
        dev.loop();
        if (!nearby && fake.nowtp.sink) {
            nearby = true;
            fake.nowtp.found("lamp-f37c", "10:52:1c:75:f3:7c");
            fake.nowtp.found("sensor-01", "24:6f:28:aa:01:02", false);
        }
    }
    return 0;
}
