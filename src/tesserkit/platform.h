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
#include <memory>
#include <string>
#include <vector>

#include "tesser/value_types.h"

namespace tesser {
class Api;
class Object;
}  // namespace tesser

namespace tesserkit {

// Something a platform task reports (Wi-Fi, SNTP, MQTT). Posted to the device,
// which hands it to every service in its own task, under the API lock.
struct Event {
    enum class Type : uint8_t {
        ScanDone,         // Wifi::scan() finished
        StaGotIp,         // the station joined and has an address
        StaDisconnected,  // value: the reason (kReasonLeft when asked to)
        ApClients,        // value: stations joined to the access point
        TimeSynced,       // SNTP set the clock
        MqttConnected,    // value: the session
        MqttDisconnected, // value: the session; text: why
        MqttMessage,      // value: the session; text: the topic; data: the payload
        NowTpPeers,       // nodes were found or lost
        NowTpChannel,     // value: the channel ESP-NOW moved to
        NowTpPairing,     // value: 0 if the pairing succeeded; text: the other node, or why not
    };
    Type type;
    int32_t value = 0;
    std::string text;
    std::string data;

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
    // Cryptographically random bytes (salts, keys).
    virtual void random(uint8_t* out, size_t len) = 0;
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
    // <hostname>.local with _http._tcp and _tesser._tcp services on `port`
    // (none if 0); off when `on` is false.
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

// A response to HttpClient::get(), its body read as it arrives.
class HttpResponse {
public:
    virtual ~HttpResponse() = default;
    virtual int status() = 0;
    // The body's length, -1 if unknown.
    virtual int64_t length() = 0;
    // Up to `len` bytes of the body: how many, 0 at its end, -1 if the
    // transfer failed or was cut short.
    virtual int read(uint8_t* buf, size_t len) = 0;
};

// A blocking HTTP(S) client for a platform task: Firmware's downloads and
// release checks.
class HttpClient {
public:
    virtual ~HttpClient() = default;
    // GETs `url`, following redirects, and hands the response (whatever its
    // status) to `fn`, whose result get() returns. Fails without calling
    // `fn` when no response came.
    virtual tesser::Check get(const char* url, const std::function<tesser::Check(HttpResponse&)>& fn) = 0;
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

// A UDP socket bound to a port: ArtNet's. Used from the device's task.
class UdpSocket {
public:
    virtual ~UdpSocket() = default;
    // The next waiting datagram, without blocking: its length (truncated to
    // `cap`), or 0 if none. `fromIp` is a.b.c.d as (a << 24) | ...
    virtual size_t receive(uint8_t* buf, size_t cap, uint32_t& fromIp, uint16_t& fromPort) = 0;
    virtual bool send(uint32_t ip, uint16_t port, const uint8_t* data, size_t len) = 0;
};

class Udp {
public:
    virtual ~Udp() = default;
    // A socket receiving on `port` (unicast and broadcast), or null.
    virtual std::unique_ptr<UdpSocket> open(uint16_t port) = 0;
};

struct MqttConfig {
    std::string uri;  // "mqtt://host:1883", "mqtts://...", "ws://...", "wss://..."
    std::string user, password, clientId;
    // Published by the broker if the connection is lost, retained.
    std::string willTopic, willPayload;
    // Subscribed to (QoS 1) on each connection.
    std::vector<std::string> subscriptions;
};

// An MQTT client: Mqtt's. One session at a time.
class MqttClient {
public:
    virtual ~MqttClient() = default;
    // Connects, and reconnects whenever the connection is lost, until
    // stop(). Events carry the session's number in `value`: MqttConnected,
    // MqttDisconnected, MqttMessage. Returns the session (> 0), or 0.
    virtual int32_t start(const MqttConfig& config, EventSink sink) = 0;
    // Publishes the will (a clean disconnection doesn't), then ends the
    // session, without blocking. Its events may still be in the device's
    // queue: they carry its number.
    virtual void stop() = 0;
    // Queues a message (QoS 0) without blocking; false if it can't.
    virtual bool publish(const std::string& topic, const std::string& payload, bool retain) = 0;
};

// Bluetooth LE: Bluetooth's. TesserAPI's BleTransport (the Nordic UART
// Service) on the API, one client at a time.
class Ble {
public:
    virtual ~Ble() = default;
    // Starts the stack and advertises under `name`. False if it couldn't.
    virtual bool begin(tesser::Api& api, const char* name) = 0;
    // Stops advertising and drops the client, or advertises again. False if
    // the transport can't do it while running (it then applies at the next
    // boot).
    virtual bool advertise(bool on) = 0;
    // The advertised name, from the next advertisement. False if it can't
    // change while running.
    virtual bool name(const char* name) = 0;
    virtual bool connected() = 0;
    // The client's address ("" if unknown or none).
    virtual std::string client() = 0;
};

struct NowTpConfig {
    std::string name;      // announced to nodes nearby (the hostname)
    uint8_t channel = 1;   // when neither the station nor the access point fixes it
    bool longRange = false;
    int8_t txPower = 0;    // dBm, 0: the driver's
    std::string pairingCode;
};

struct NowTpPeer {
    std::string name;
    std::string mac;      // "aa:bb:cc:dd:ee:ff"
    int8_t rssi = 0;
    uint8_t channel = 0;
    bool tesser = false;  // a TesserAPI node, mounted under /peers when asked
    bool authenticated = false;
};

struct NowTpStatus {
    bool started = false;
    uint8_t channel = 0;
    bool secured = false;
    bool timeSynced = false, timeReference = false;
    int64_t timeOffsetUs = 0;
    uint32_t sent = 0, received = 0, failed = 0;
};

// ESP-NOW through NowTP: the NowTp service's. The API is served to the nodes
// nearby (TesserAPI's NowTpTransport), which are mounted under a parent
// object when one is given.
class NowTpRadio {
public:
    virtual ~NowTpRadio() = default;
    // Starts ESP-NOW on the running Wi-Fi, discovery and the API's transport.
    // Events: NowTpPeers, NowTpChannel, NowTpPairing.
    virtual bool begin(tesser::Api& api, tesser::Object* peers, const NowTpConfig& config, EventSink sink) = 0;
    virtual void end() = 0;
    virtual std::vector<NowTpPeer> peers() = 0;
    virtual NowTpStatus status() = 0;
    // Changes the channel ESP-NOW uses when nothing fixes it, telling the
    // nodes nearby; false if an access point fixes it.
    virtual bool channel(uint8_t ch) = 0;
    virtual bool longRange(bool on) = 0;
    // Asynchronous: Event::NowTpPairing. Joins the installation of a node
    // whose pairing window is open, or opens ours for `timeoutMs`.
    virtual bool pair(uint32_t timeoutMs) = 0;
    virtual bool acceptPairing(uint32_t timeoutMs) = 0;
    // Forgets the installation key, here and in storage, started or not:
    // unsecured again.
    virtual bool forgetKey() = 0;
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
    Udp* udp = nullptr;
    MqttClient* mqtt = nullptr;
    Ble* ble = nullptr;
    NowTpRadio* nowtp = nullptr;
};

// The ESP implementations (null on the host).
Core* espCore();
Wifi* espWifi();
Mdns* espMdns();
Clock* espClock();
Ota* espOta();
HttpClient* espHttpClient();
Coredump* espCoredump();
Udp* espUdp();
MqttClient* espMqtt();     // null if the build has no MQTT client
MqttClient* espMqttTls();  // the same, verifying brokers with the certificate bundle
Ble* espBle();             // null if the build has no Bluetooth LE (NimBLE, or Arduino's BLE)
NowTpRadio* espNowTp();    // null if the build has no NowTP

}  // namespace tesserkit
