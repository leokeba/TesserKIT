#pragma once

// The platform layer (docs/DESIGN.md section 3.3): what services need from
// the chip, as narrow interfaces. src/tesserkit/platform/esp/ implements them
// on ESP-IDF (which Arduino-ESP32 3.x ships); test/fake_platform.* in memory.
//
// Each service takes the interfaces it uses from the device's Platform, and
// installs the ESP implementation when none was given, so an interface whose
// service isn't instantiated isn't linked.

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include "tesser/value_types.h"

namespace tesserkit {

// Something a platform task reports (Wi-Fi, SNTP). Posted to the device,
// which hands it to every service in its own task, under the API lock.
struct Event {
    enum class Type : uint8_t {
        ScanDone,         // Wifi::scan() finished
        StaGotIp,         // the station joined and has an address
        StaDisconnected,  // value: the reason (kReasonLeft when asked to)
        ApClients,        // value: stations joined to the access point
        TimeSynced,       // SNTP set the clock
    };
    Type type;
    int32_t value = 0;
    std::string text;

    // StaDisconnected's reason after Wifi::disconnect() (ESP-IDF's
    // WIFI_REASON_ASSOC_LEAVE): not a failure.
    static constexpr int32_t kReasonLeft = 8;
};
using EventSink = std::function<void(Event)>;

enum class ResetReason : uint8_t { PowerOn, Software, Panic, Watchdog, Brownout, DeepSleep, Other };

// Facts fixed at boot.
struct BootInfo {
    std::string chip;        // "ESP32-D0WD-V3 rev 3.0, 2 cores, 240 MHz"
    std::string target;      // "esp32"
    uint16_t imageChipId = 0;  // the chip id app images must declare
    uint32_t flashSize = 0;    // bytes
    std::string idf;         // ESP-IDF version
    uint8_t mac[6] = {};     // station MAC
    ResetReason resetReason = ResetReason::Other;
    bool psram = false;
    // The running image.
    std::string version, project, built;
    std::string slot;        // "ota_0"
    uint32_t slotSize = 0;   // bytes
    uint32_t appSize = 0;    // bytes
};

// The chip: always present.
class Core {
public:
    virtual ~Core() = default;
    virtual void info(BootInfo& out) = 0;
    virtual uint64_t uptimeMs() = 0;
    virtual uint32_t freeHeap() = 0;
    virtual uint32_t minFreeHeap() = 0;
    virtual uint32_t freePsram() = 0;
    // False on chips without a sensor.
    virtual bool temperature(float& celsius) = 0;
    virtual void restart() = 0;
    // Runs `fn` in a task of its own (downloads), which ends with it.
    virtual bool spawn(const char* name, uint32_t stackBytes, std::function<void()> fn) = 0;
};

struct ScanResult {
    std::string ssid;
    int8_t rssi = 0;
    uint8_t channel = 0;
    bool secure = false;
};

struct StaConfig {
    std::string ssid, password;
    bool staticIp = false;
    uint32_t ip = 0, mask = 0, gateway = 0, dns = 0;  // a.b.c.d as (a << 24) | ...
};

struct StaInfo {
    std::string ssid;
    uint32_t ip = 0;
    int8_t rssi = 0;
    uint8_t channel = 0;
    uint8_t bssid[6] = {};
};

struct ApConfig {
    std::string ssid, password;  // an empty password makes an open network
    uint8_t channel = 1;
    bool hidden = false;
};

// Wi-Fi, IP and mDNS: Network's.
class Wifi {
public:
    virtual ~Wifi() = default;
    // Starts the radio in station mode; events go to `sink` from then on.
    virtual void begin(const char* hostname, EventSink sink) = 0;
    virtual void hostname(const char* name) = 0;
    virtual void txPower(int8_t dBm) = 0;
    // Asynchronous: Event::ScanDone, then scanResults().
    virtual bool scan() = 0;
    virtual std::vector<ScanResult> scanResults() = 0;
    // Asynchronous: Event::StaGotIp or Event::StaDisconnected.
    virtual bool connect(const StaConfig& config) = 0;
    virtual void disconnect() = 0;
    // False while not connected.
    virtual bool staInfo(StaInfo& out) = 0;
    virtual bool startAp(const ApConfig& config) = 0;
    virtual void stopAp() = 0;
    virtual uint32_t apIp() = 0;
    // A DNS server answering every name with the access point's address, and
    // the HTTP server's 404 handler redirecting to the interface.
    virtual void captivePortal(bool on) = 0;
};

// mDNS: Network's, unless built with TESSERKIT_NO_MDNS.
class Mdns {
public:
    virtual ~Mdns() = default;
    // <hostname>.local with an _http._tcp service on `port` (none if 0);
    // off when `on` is false.
    virtual void set(bool on, const char* hostname, const char* version, uint16_t port) = 0;
};

// The clock and SNTP: Time's.
class Clock {
public:
    virtual ~Clock() = default;
    virtual void begin(EventSink sink) = 0;
    virtual int64_t epochMs() = 0;
    virtual void setEpochMs(int64_t ms) = 0;
    // A POSIX TZ string, applied to localtime() everywhere in the firmware.
    virtual void timezone(const char* posix) = 0;
    // Asynchronous: Event::TimeSynced on each synchronization.
    virtual void sntp(bool on, const char* server) = 0;
};

// The OTA slots: Firmware's. One update at a time.
class Ota {
public:
    virtual ~Ota() = default;
    // Size of the slot the next update goes to.
    virtual uint32_t nextSlotSize() = 0;
    // Starts writing the next slot; `size` 0 if unknown.
    virtual tesser::Check open(uint32_t size) = 0;
    virtual tesser::Check write(const uint8_t* data, size_t len) = 0;
    // Verifies the image and makes it boot next.
    virtual tesser::Check finish() = 0;
    virtual void abort() = 0;
    // The running image is new and not confirmed yet (rollback enabled).
    virtual bool pending() = 0;
    virtual bool confirm() = 0;
    // Makes the previous image boot next.
    virtual tesser::Check rollback() = 0;
};

// A blocking HTTP(S) client for a platform task: Firmware's downloads.
class HttpClient {
public:
    virtual ~HttpClient() = default;
    // GETs `url`: `length` once the headers are in (-1 if unknown), then
    // `data` for each chunk; either may stop the transfer by returning a
    // failure, which get() returns.
    virtual tesser::Check get(const char* url, const std::function<tesser::Check(int64_t length)>& length,
                              const std::function<tesser::Check(const uint8_t* data, size_t len)>& data) = 0;
};

// The core dump partition: System's.
class Coredump {
public:
    virtual ~Coredump() = default;
    // Bytes of the stored dump, 0 if none.
    virtual uint32_t size() = 0;
    virtual int read(uint32_t offset, uint8_t* buf, size_t len) = 0;
    virtual bool erase() = 0;
};

// What a device runs on. Null members are filled with the ESP
// implementations by the services that use them.
struct Platform {
    Core* core = nullptr;
    Wifi* wifi = nullptr;
    Mdns* mdns = nullptr;
    Clock* clock = nullptr;
    Ota* ota = nullptr;
    HttpClient* http = nullptr;
    Coredump* coredump = nullptr;  // stays null without a core dump partition
};

// The ESP implementations (null on the host).
Core* espCore();
Wifi* espWifi();
Mdns* espMdns();
Clock* espClock();
Ota* espOta();
HttpClient* espHttpClient();
Coredump* espCoredump();

}  // namespace tesserkit
