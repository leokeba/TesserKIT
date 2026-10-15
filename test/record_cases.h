// The worst-case settings records against the NVS partition (docs/DESIGN.md
// section 11, "Settings in NVS"): every service at its limits, every
// persisted string at its longest.
#pragma once

#include <stdio.h>

#include "rig.h"
#include "tesserkit/limits.h"
#include "tesserkit/settings_store.h"

namespace record_cases {

using rig::Status;
namespace limits = tesserkit::limits;

// Every service that persists something in /sys, plus the Sequencer.
struct FullRig : rig::Rig {
    tesserkit::System system{dev};
    tesserkit::Network network{dev};
    tesserkit::Time time{dev};
    tesserkit::Firmware firmware{dev};
    tesserkit::Files files{dev};
    tesserkit::Backup backup{dev};
    tesserkit::Timers timers{dev};
    tesserkit::Auth auth{dev};
    tesserkit::Mqtt mqtt{dev};
    tesserkit::ArtNet artnet{dev};
    tesserkit::Bluetooth bluetooth{dev};
    tesserkit::NowTp nowtp{dev};
    tesserkit::Sequencer sequencer{dev};
    tesserkit::Log log{dev};
    tesserkit::Battery battery{dev};
    tesserkit::Sleep sleep{dev};
    tesserkit::UdpApi udp{dev};
    tesserkit::Peers peers{dev};
    bool on = false;
    std::string longName = std::string(limits::kPath - 4, 'n');  // "/" + it + "/on": limits::kPath
    std::string longPath = "/" + longName + "/on";

    // `appStorage` false: the device's default storage (the filesystem).
    explicit FullRig(bool appStorage = true) : rig::Rig(true, appStorage) {
        files.root(fake.fs.dir.c_str());
        firmware.github("leokeba/TesserKIT", "tesserkit-minimal-{target}.bin");
        network.ethernet(tesserkit::boards::gledopto());
        dev.api().object(longName.c_str()).value("on", on);  // names must outlive the tree
    }
    ~FullRig() { CHECK(rig::drainBackground()); }
};

std::string chars(size_t n, char c = 'x') { return std::string(n, c); }
std::string quoted(const std::string& s) { return "\"" + s + "\""; }
// A field holding JSON text (timers' and steps' values): a JSON string of n
// characters, quotes included, itself as a JSON string.
std::string jsonValue(size_t n) { return "\"\\\"" + chars(n - 2, 'v') + "\\\"\""; }
std::string field(const char* key, const std::string& json) { return quoted(key) + ":" + json; }

// The longest valid IPv4 address and netmask.
const char* kIp = "\"192.168.100.200\"";
const char* kMask = "\"255.255.255.252\"";

std::string staticBlock() {
    return "{\"enabled\":true,\"ip\":" + std::string(kIp) + ",\"mask\":" + kMask + ",\"gateway\":" + kIp +
           ",\"dns\":" + kIp + "}";
}

// Fills every persisted setting at its limits; false (with the failing
// write's path in `failed`) if a write was refused.
bool fill(FullRig& r, std::string& failed) {
    auto set = [&r, &failed](const std::string& path, const std::string& body) {
        rig::Reply rep = r.set(path, body);
        if (rep.status != Status::Ok) {
            failed = path + ": " + rep.body;
            return false;
        }
        return true;
    };
    bool ok = true;
    // Network.
    ok = ok && set("/sys/network/hostname", quoted(chars(32, 'h')));
    ok = ok && set("/sys/network/ethernet/enabled", "false");
    ok = ok && set("/sys/network/ethernet/static", staticBlock());
    {
        std::string list = "[";
        for (int i = 0; i < 5; i++) {
            if (i) list += ",";
            std::string ssid = chars(31, 's') + std::to_string(i);
            list += "{" + field("ssid", quoted(ssid)) + "," + field("password", quoted(chars(64, 'p'))) + "," +
                    field("bssid", "\"3a:07:16:43:fd:24\"") + "," + field("static", staticBlock()) + "}";
        }
        ok = ok && set("/sys/network/wifi/networks", list + "]");
    }
    ok = ok && set("/sys/network/wifi/mode", "\"never\"");
    ok = ok && set("/sys/network/wifi/powerSave", "\"none\"");
    ok = ok && set("/sys/network/ap", "{\"mode\":\"never\",\"ssid\":" + quoted(chars(32, 'a')) +
                                          ",\"password\":" + quoted(chars(63, 'p')) +
                                          ",\"channel\":13,\"hidden\":true}");
    ok = ok && set("/sys/network/mdns", "false");
    // Time.
    ok = ok && set("/sys/time/tz", quoted(chars(63, 'T')));
    ok = ok && set("/sys/time/sntp", "{\"enabled\":false,\"server\":" + quoted(chars(limits::kName, 's')) + "}");
    // Firmware, Backup.
    ok = ok && set("/sys/firmware/github/autoCheck", "false");
    ok = ok && set("/sys/backup/restartAfter", "false");
    ok = ok && set("/sys/backup/keep", "50");
    // Timers, at their longest: a run's result is cut to limits::kResult.
    {
        std::string list = "[";
        for (int i = 0; i < 16; i++) {
            if (i) list += ",";
            std::string name = chars(22, 't') + (i < 10 ? "0" : "") + std::to_string(i);
            list += "{" + field("name", quoted(name)) + ",\"enabled\":false,\"time\":\"23:59\",\"days\":127," +
                    field("path", quoted(r.longPath)) + "," + field("value", jsonValue(limits::kValue)) + "}";
        }
        ok = ok && set("/sys/timers", list + "]");
    }
    ok = ok && set("/sys/timersCatchUp", "true");
    // Auth: 8 accounts, their hashes (the longest a password gives).
    {
        std::string list = "[";
        for (int i = 0; i < 8; i++) {
            if (i) list += ",";
            std::string name = chars(23, 'u') + std::to_string(i);
            list += "{" + field("name", quoted(name)) + "," + field("password", quoted(chars(64, 'p'))) +
                    ",\"role\":\"admin\"}";
        }
        ok = ok && set("/sys/auth/users", list + "]");
        CHECK(rig::drainBackground());  // the hashes
    }
    ok = ok && set("/sys/auth/sessionHours", "720");
    // MQTT.
    {
        std::string publish = "[";
        for (size_t i = 0; i < tesserkit::Mqtt::kMaxPublish; i++) {
            if (i) publish += ",";
            publish += "{" + field("path", quoted(r.longPath)) + ",\"interval\":86400000}";
        }
        publish += "]";
        ok = ok && set("/sys/mqtt",
                       "{\"enabled\":false," + field("uri", quoted("mqtts://" + chars(limits::kUri - 8, 'b'))) + "," +
                           field("user", quoted(chars(limits::kName, 'u'))) + "," +
                           field("password", quoted(chars(limits::kLongSecret, 'p'))) + "," +
                           field("clientId", quoted(chars(limits::kName, 'c'))) + "," +
                           field("prefix", quoted(chars(limits::kName, 'f'))) + "," + field("publish", publish) +
                           ",\"homeAssistant\":true," + field("discoveryPrefix", quoted(chars(limits::kName, 'd'))) +
                           "}");
    }
    // Art-Net.
    {
        std::string list = "[";
        for (size_t i = 0; i < tesserkit::ArtNet::kMaxMappings; i++) {
            if (i) list += ",";
            list += "{" + field("path", quoted(r.longPath)) +
                    ",\"coarse\":512,\"fine\":511,\"min\":-1.2345678901234567e+300,"
                    "\"max\":1.2345678901234567e+300,\"threshold\":255}";
        }
        ok = ok && set("/sys/artnet", "{\"enabled\":true,\"universe\":32767,\"mappings\":" + list + "]}");
    }
    ok = ok && set("/sys/bluetooth/enabled", "false");
    // NowTP.
    ok = ok && set("/sys/nowtp", "{\"enabled\":false,\"mountPeers\":false,\"channel\":13,\"longRange\":true," +
                                     field("pairingCode", quoted(chars(limits::kSecret, 'c'))) + "}");
    // Log, Battery, Sleep, UDP.
    ok = ok && set("/sys/log/level", "\"error\"");
    ok = ok && set("/sys/battery/low", "50");
    ok = ok && set("/sys/sleep", "{\"wakePin\":39,\"wakeLevel\":\"high\",\"wakePull\":true}");
    ok = ok && set("/sys/udp", "{\"enabled\":false,\"port\":65535,\"interfaces\":3,\"peerAccess\":\"public\","
                               "\"mountPeers\":false}");
    // Peers: 16 nodes' settings.
    const char* order = "\"udp/ethernet, udp/wifi, udp/ap, udp, nowtp\"";
    ok = ok && set("/sys/peers/order", order);
    ok = ok && set("/sys/peers/account", "{" + field("name", quoted(chars(limits::kName, 'a'))) + "," +
                                             field("password", quoted(chars(limits::kSecret, 'p'))) + "}");
    {
        std::string list = "[";
        for (int i = 0; i < 16; i++) {
            if (i) list += ",";
            char id[18];
            snprintf(id, sizeof(id), "10:52:1c:75:f3:%02x", i);
            list += "{" + field("id", quoted(id)) + "," + field("name", quoted(chars(limits::kName, 'n'))) + "," +
                    field("order", order) + "," + field("address", "\"192.168.100.200:65535\"") + "," +
                    field("user", quoted(chars(limits::kName, 'u'))) + "," +
                    field("password", quoted(chars(limits::kSecret, 'p'))) + "}";
        }
        ok = ok && set("/sys/peers/nodes", list + "]");
    }
    // The Sequencer: its own record.
    {
        std::string list = "[";
        for (int i = 0; i < 32; i++) {
            if (i) list += ",";
            std::string name = chars(22, 's') + (i < 10 ? "0" : "") + std::to_string(i);
            list += "{" + field("name", quoted(name)) + "," + field("path", quoted(r.longPath)) + "," +
                    field("value", jsonValue(limits::kValue)) + ",\"wait\":\"time\",\"ms\":86400000," +
                    field("next", quoted(chars(24, 'x'))) + "}";
        }
        ok = ok && set("/sequencer/steps", list + "]");
    }
    ok = ok && set("/sequencer/boot", quoted(chars(24, 'x')));
    ok = ok && set("/sequencer/finally",
                   "{" + field("path", quoted(r.longPath)) + "," + field("value", jsonValue(limits::kValue)) + "}");
    return ok;
}

// Measured with no limit of NVS's: what every setting at its limits takes,
// recorded in DESIGN.md section 11. A change that grows it fails here first.
constexpr size_t kWorstSys = 16709, kWorstSequencer = 10288;

TEST(record_worst_case) {
    FullRig r;
    CHECK(r.dev.begin());
    std::string failed;
    CHECK(fill(r, failed));
    if (!failed.empty()) printf("refused: %s\n", failed.c_str());
    r.run(2500);
    size_t sys = r.storage.records["sys"].size(), sequencer = r.storage.records["sequencer"].size();
    if (getenv("RECORD_SIZES")) {
        for (const auto& kv : r.storage.records) printf("record %s: %zu bytes\n", kv.first.c_str(), kv.second.size());
    }
    CHECK(sys <= kWorstSys);
    CHECK(sequencer <= kWorstSequencer);
    // What it needs: NVS rewrites a record beside the others.
    using tesserkit::SettingsStore;
    CHECK(!SettingsStore::fits(20480, sys, sequencer));     // the tables' 20 KB
    CHECK(!SettingsStore::fits(32768, sys, sequencer));
    CHECK(SettingsStore::fits(65536, sys, sequencer));      // 64 KB holds it
    CHECK(SettingsStore::fits(65536, sequencer, sys));
}

// On a board's 20 KB of NVS, a record it couldn't rewrite isn't written: the
// stored one stays, the status says so, and NVS isn't asked again every
// 50 ms. Once it fits again, it is saved.
TEST(record_too_large_for_nvs_is_refused) {
    FullRig r;
    r.dev.settingsCapacity(20480);
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/time/tz", "\"EST5EDT\"").status, Status::Ok);
    r.run(2500);
    std::string stored = r.storage.records["sys"];
    CHECK(stored.find("EST5EDT") != std::string::npos);
    CHECK_EQ(r.value("/sys/status/settings"), "\"\"");
    int saves = r.storage.saves;

    std::string failed;
    CHECK(fill(r, failed));
    r.run(2500);
    CHECK_EQ(r.storage.records["sys"], stored);  // the stored record stays
    CHECK(r.value("/sys/status/settings").find("not saved") != std::string::npos);
    int after = r.storage.saves;
    r.run(5000);
    CHECK_EQ(r.storage.saves, after);  // not tried again until it changes
    CHECK(after - saves <= 2);         // the sequencer's, perhaps: it fits beside the stored sys

    // Smaller again: saved, and the warning goes.
    CHECK_EQ(r.set("/sys/timers", "[]").status, Status::Ok);
    CHECK_EQ(r.set("/sys/peers/nodes", "[]").status, Status::Ok);
    CHECK_EQ(r.set("/sys/artnet/mappings", "[]").status, Status::Ok);
    CHECK_EQ(r.set("/sys/mqtt/publish", "[]").status, Status::Ok);
    CHECK_EQ(r.set("/sequencer/steps", "[]").status, Status::Ok);
    r.run(2500);
    CHECK(r.storage.records["sys"] != stored);
    CHECK(r.storage.records["sys"].find("EST5EDT") == std::string::npos);
    CHECK_EQ(r.value("/sys/status/settings"), "\"\"");
}

// On the filesystem (docs/DESIGN.md section 6.1), the worst case NVS refuses
// is saved, and loaded at the next boot.
TEST(record_worst_case_on_the_filesystem) {
    std::string sys, sequencer;
    {
        FullRig nvs;
        nvs.dev.settingsCapacity(20480);
        CHECK(nvs.dev.begin());
        std::string failed;
        CHECK(fill(nvs, failed));
        nvs.run(2500);
        CHECK(nvs.value("/sys/status/settings").find("not saved") != std::string::npos);
    }
    FullRig r(false);
    r.dev.settingsCapacity(20480);  // NVS's: not the filesystem's
    CHECK(r.dev.begin());
    CHECK(r.dev.settingsOnFiles());
    std::string failed;
    CHECK(fill(r, failed));
    r.run(2500);
    CHECK_EQ(r.value("/sys/status/settings"), "\"\"");
    auto read = [&r](const char* rel) {
        std::string out;
        if (FILE* f = fopen((r.fake.fs.dir + "/.settings/" + rel).c_str(), "rb")) {
            char buf[512];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
            fclose(f);
        }
        return out;
    };
    sys = read("sys.json");
    sequencer = read("sequencer.json");
    CHECK(sys.size() > tesserkit::SettingsStore::largest(20480, 0));
    CHECK(sys.size() <= kWorstSys);
    CHECK(sequencer.size() > 8000);
    CHECK(r.fake.nvs.records.empty());
    // Read back whole.
    tesser::MemoryStorage copy;
    copy.records["sys"] = sys;
    copy.records["sequencer"] = sequencer;
    FullRig again;
    again.storage.records = copy.records;
    CHECK(again.dev.begin());
    CHECK_EQ(again.value("/sys/network/hostname"), r.value("/sys/network/hostname"));
    CHECK_EQ(again.value("/sequencer/steps"), r.value("/sequencer/steps"));
}

// What makes the worst case the worst: one character more than each limit is
// refused, and the refused write changes nothing.
TEST(record_string_limits) {
    FullRig r;
    CHECK(r.dev.begin());
    std::string failed;
    CHECK(fill(r, failed));
    r.run(2500);
    std::map<std::string, std::string> before = r.storage.records;
    std::string longer = r.longPath + "x";  // resolves no more, but is refused for its length first
    struct Over {
        std::string path, body;
    };
    std::vector<Over> over = {
        {"/sys/network/ap/ssid", quoted(chars(33))},
        {"/sys/time/sntp/server", quoted(chars(limits::kHost + 1))},
        {"/sys/timers/tttttttttttttttttttttt00/path", quoted(longer)},
        {"/sys/timers/tttttttttttttttttttttt00/value", jsonValue(limits::kValue + 1)},
        {"/sys/mqtt/uri", quoted("mqtt://" + chars(limits::kUri - 6))},
        {"/sys/mqtt/user", quoted(chars(limits::kSecret + 1))},
        {"/sys/mqtt/password", quoted(chars(limits::kLongSecret + 1))},
        {"/sys/mqtt/clientId", quoted(chars(limits::kSecret + 1))},
        {"/sys/mqtt/prefix", quoted(chars(limits::kSecret + 1))},
        {"/sys/mqtt/discoveryPrefix", quoted(chars(limits::kName + 1))},
        {"/sys/mqtt/publish", "[{\"path\":" + quoted(longer) + "}]"},
        {"/sys/artnet/mappings", "[{\"path\":" + quoted(longer) + "}]"},
        {"/sys/nowtp/pairingCode", quoted(chars(limits::kSecret + 1))},
        {"/sys/peers/account/name", quoted(chars(limits::kName + 1))},
        {"/sys/peers/account/password", quoted(chars(limits::kSecret + 1))},
        {"/sys/peers/nodes/10:52:1c:75:f3:00/name", quoted(chars(limits::kName + 1))},
        {"/sys/peers/nodes/10:52:1c:75:f3:00/user", quoted(chars(limits::kName + 1))},
        {"/sys/peers/nodes/10:52:1c:75:f3:00/password", quoted(chars(limits::kSecret + 1))},
        {"/sequencer/steps/0/path", quoted(longer)},
        {"/sequencer/steps/0/value", jsonValue(limits::kValue + 1)},
        {"/sequencer/steps/0/next", quoted(chars(25))},
        {"/sequencer/boot", quoted(chars(25))},
        {"/sequencer/finally/path", quoted(longer)},
        {"/sequencer/finally/value", jsonValue(limits::kValue + 1)},
    };
    for (const Over& o : over) {
        rig::Reply rep = r.set(o.path, o.body);
        if (!CHECK_EQ(rep.status, Status::InvalidValue)) printf("accepted: %s\n", o.path.c_str());
    }
    r.run(2500);
    CHECK(r.storage.records == before);
}

}  // namespace record_cases
