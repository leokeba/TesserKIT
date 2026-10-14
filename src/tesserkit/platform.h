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
class DatagramEndpoint;
class Object;
class UdpLink;
}  // namespace tesser

namespace tesserkit {

// Something a platform task reports (Wi-Fi, SNTP, MQTT). Posted to the device,
// which hands it to every service in its own task, under the API lock. The
// device's queue is bounded (Device::post()), so an event can be lost: a
// service keeps no state that only an edge would correct, and reads the
// platform's state back instead (Network's link and addresses).
struct Event {
    enum class Type : uint8_t {
        ScanDone,         // Wifi::scan() finished
        StaAssociated,    // the station joined a network; its address comes next (StaGotIp)
        StaGotIp,         // the station joined and has an address
        StaDisconnected,  // value: the reason (kReasonLeft when asked to)
        ApClients,        // value: stations joined to the access point
        TimeSynced,       // SNTP set the clock
        MqttConnected,    // value: the session
        MqttDisconnected, // value: the session; text: why
        MqttMessage,      // value: the session; text: the topic; data: the payload (at most 8 queued)
        NowTpPeers,       // nodes were found or lost (one queued at most)
        NowTpChannel,     // value: the channel ESP-NOW moved to
        NowTpPairing,     // value: 0 if the pairing succeeded; text: the other node, or why not
        EthLink,          // value: 1 when the cable's link came up, 0 when it went down
        EthGotIp,         // Ethernet has an address
        PeersChanged,     // a node under /peers was mounted, got a route, or changed state or route (one queued at most)
        DeviceLost,       // text: the MAC of a device an endpoint lost (Peers::watch()); value: tesser::PeerLoss
        PeerRefused,      // text: the MAC of a device that refused this device's token
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
    // The largest block one allocation can get: a fragmented heap aborts on
    // a large one with plenty free in all.
    virtual uint32_t largestFreeBlock() = 0;
    virtual uint32_t freePsram() = 0;
    // False on chips without a sensor.
    virtual bool temperature(float& celsius) = 0;
    virtual void restart() = 0;
    // Cryptographically random bytes (salts, keys).
    virtual void random(uint8_t* out, size_t len) = 0;
    // Runs `fn` in a task of its own (downloads), which ends with it.
    virtual bool spawn(const char* name, uint32_t stackBytes, std::function<void()> fn) = 0;
};

// One access point a scan heard.
struct ScanResult {
    std::string ssid;
    uint8_t bssid[6] = {};
    int8_t rssi = 0;
    uint8_t channel = 0;
    bool secure = false;
};

// An interface's IPv4 setup: DHCP, or a static address.
struct IpConfig {
    bool staticIp = false;
    uint32_t ip = 0, mask = 0, gateway = 0, dns = 0;  // a.b.c.d as (a << 24) | ...

    bool operator==(const IpConfig& o) const {
        return staticIp == o.staticIp && ip == o.ip && mask == o.mask && gateway == o.gateway && dns == o.dns;
    }
    bool operator!=(const IpConfig& o) const { return !(*this == o); }
};

struct StaConfig : IpConfig {
    std::string ssid, password;
    // Only this access point of the network (several can share its name).
    bool bssidSet = false;
    uint8_t bssid[6] = {};
};

struct StaInfo {
    std::string ssid;
    uint32_t ip = 0;
    int8_t rssi = 0;
    uint8_t channel = 0;
    uint8_t bssid[6] = {};
};

// The station's power save: none (lowest latency), modem sleep between
// beacons (ESP-IDF's default), or maximum (sleeps through several).
enum class PowerSave : uint8_t { None, Min, Max };

struct ApConfig {
    std::string ssid, password;  // an empty password makes an open network
    uint8_t channel = 1;
    bool hidden = false;
};

// Wi-Fi and IP: Network's.
class Wifi {
public:
    virtual ~Wifi() = default;
    // Starts the radio in station mode; events go to `sink` from then on.
    virtual void begin(const char* hostname, EventSink sink) = 0;
    virtual void hostname(const char* name) = 0;
    virtual void txPower(int8_t dBm) = 0;
    virtual void powerSave(PowerSave mode) = 0;
    // Asynchronous: Event::ScanDone, then scanResults(): one per access
    // point heard, hidden networks left out.
    virtual bool scan() = 0;
    virtual std::vector<ScanResult> scanResults() = 0;
    // Asynchronous: Event::StaAssociated then Event::StaGotIp, or
    // Event::StaDisconnected.
    virtual bool connect(const StaConfig& config) = 0;
    virtual void disconnect() = 0;
    // False while not connected: the station's state as the platform keeps
    // it, whatever events the device's queue lost.
    virtual bool staInfo(StaInfo& out) = 0;
    virtual bool startAp(const ApConfig& config) = 0;
    virtual void stopAp() = 0;
    virtual uint32_t apIp() = 0;
    // Stations joined to the access point now (0 while it is down).
    virtual int32_t apClients() = 0;
    // A DNS server answering every name with the access point's address, and
    // the HTTP server's 404 handler redirecting to the interface.
    virtual void captivePortal(bool on) = 0;
};

// How a board wires its Ethernet PHY to the ESP32's internal MAC (RMII):
// fixed by the hardware. Presets for known boards are in network.h.
struct EthernetBoard {
    // PHYs the driver knows by name; any other works as Generic (the
    // registers IEEE 802.3 standardizes).
    enum class Phy : uint8_t { Generic, Lan8720, Ip101, Rtl8201, Dp83848, Ksz8081 };
    // The 50 MHz RMII clock: from an oscillator on the board into GPIO 0, or
    // out of the ESP32 on GPIO 0, 16 or 17 (unstable with Wi-Fi on, the
    // ESP32's errata say).
    enum class Clock : uint8_t { In, Out };

    Phy phy = Phy::Generic;
    int8_t address = -1;  // on the MDIO bus; -1 finds it
    int8_t mdc = 23, mdio = 18;
    int8_t power = -1;  // a GPIO powering the PHY or its oscillator, -1 if none
    Clock clock = Clock::In;
    int8_t clockGpio = 0;
};

struct EthInfo {
    bool link = false;
    uint32_t ip = 0;     // 0 until it has one
    uint16_t mbps = 0;   // 10 or 100 with a link
    bool fullDuplex = false;
    uint8_t mac[6] = {};
};

// Ethernet: Network's, when the application declares a board.
class Ethernet {
public:
    virtual ~Ethernet() = default;
    // Starts the MAC and the PHY; events go to `sink` from then on: EthLink,
    // EthGotIp. False if the hardware didn't answer.
    virtual bool begin(const EthernetBoard& board, const char* hostname, const IpConfig& ip, EventSink sink) = 0;
    virtual void hostname(const char* name) = 0;
    // DHCP or a static address, from now on. With DHCP, an address may
    // also appear without an event: a link-local one (169.254/16), where the
    // stack has AutoIP, while DHCP keeps asking.
    virtual void address(const IpConfig& ip) = 0;
    // Stops the MAC and the PHY (no link), or starts them again.
    virtual void enabled(bool on) = 0;
    // False before begin(). The link and the address as they are now,
    // whatever events the device's queue lost.
    virtual bool info(EthInfo& out) = 0;
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

// A flash filesystem (LittleFS): Files'. Files themselves are read and written
// through stdio and dirent below the mount point, the same on the host.
class Filesystem {
public:
    virtual ~Filesystem() = default;
    // Mounts the partition labelled `label` (null: "files", else "spiffs")
    // at `root`, formatting one that holds no filesystem. True if it is
    // mounted, by this call or before it (an application's LittleFS.begin()).
    virtual bool mount(const char* label, const char* root) = 0;
    // Bytes of the partition, and in use.
    virtual bool usage(uint64_t& total, uint64_t& used) = 0;
    // Erases every file; mounted again after.
    virtual bool format() = 0;
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
    // Starts the stack and advertises under `name`, again after end().
    // False if it couldn't.
    virtual bool begin(tesser::Api& api, const char* name) = 0;
    // Stops the stack and gives its memory back, dropping the client. False
    // if the transport can't (advertise(false) is what's left).
    virtual bool end() = 0;
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
    size_t maxCalls = 8;   // calls in flight to other nodes (one per node streamed to)
    // false: no worker task; the transport's messages are handled in poll(),
    // and `queued` runs (from NowTP's task) when one arrives.
    bool runTask = true;
    std::function<void()> queued;
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
    // TesserAPI's endpoint: null before the first begin(), then kept.
    virtual tesser::DatagramEndpoint* endpoint() = 0;
    // Without a worker task (NowTpConfig::runTask false): handles what
    // arrived, and expires calls. In the device's task.
    virtual void poll() = 0;
};

// TesserAPI's API over UDP (its UdpTransport): UdpApi's. The protocol is
// TesserAPI's own tesser::UdpLink, platform-independent; this owns its
// socket and tasks.
class UdpApiTransport {
public:
    virtual ~UdpApiTransport() = default;
    // Opens the socket on `port` and starts the link, advertising as
    // `name`; the transport is created on the first call. False if it
    // couldn't.
    // `maxCalls`: calls in flight to other nodes, fixed when the transport
    // is created. Without `ownTask`, no worker task: the messages are
    // handled in poll(), and `queued` runs (from the receive task) when one
    // arrives.
    // `interfaces`: a mask of TesserAPI's UdpTransport::kEthernet, kWifi and
    // kAp, the only interfaces used (one: the socket is bound to it).
    virtual bool begin(tesser::Api& api, uint16_t port, const std::string& name, size_t maxCalls, bool ownTask,
                       std::function<void()> queued, uint8_t interfaces) = 0;
    // Without a worker task: handles what arrived, retransmits, advertises.
    // In the device's task, when `queued` ran and at least every 20 ms.
    virtual void poll() = 0;
    // Says goodbye, closes the socket and forgets the peers; begin() again
    // to restart. Safe from a request that came over UDP.
    virtual void end() = 0;
    // The link: null before the first begin().
    virtual tesser::UdpLink* link() = 0;
};

// The log's output (ESP-IDF's esp_log, which TesserAPI's warnings use too):
// Log's. Lines still go to the console; captured ones wait here for the
// device's task.
class LogOutput {
public:
    virtual ~LogOutput() = default;
    // Captures the log's lines from now on, keeping up to `bytes` of them
    // until next() takes them; more are counted in dropped(). False if it
    // can't.
    virtual bool begin(size_t bytes) = 0;
    // The level the whole firmware logs at, the console included: 0 none,
    // 1 error, 2 warn, 3 info, 4 debug, 5 verbose.
    virtual uint8_t level() = 0;
    virtual void level(uint8_t level) = 0;
    // The most verbose level the build keeps (CONFIG_LOG_MAXIMUM_LEVEL):
    // above it, lines aren't compiled in.
    virtual uint8_t maxLevel() = 0;
    // The oldest captured line, without colours or its newline: "W (1234)
    // wifi: ...". False if there is none.
    virtual bool next(std::string& line) = 0;
    // Lines lost because nothing took them in time.
    virtual uint32_t dropped() = 0;
    // While muted, what the calling task logs isn't captured (the console
    // still has it): the Log service's own emission can't feed itself.
    virtual void mute(bool on) = 0;
};

// How the device last woke from deep sleep.
enum class WakeCause : uint8_t { None, Timer, Pin, Other };

// Deep sleep: Sleep's.
class Power {
public:
    virtual ~Power() = default;
    // Why this boot happened, if it was a wake from deep sleep.
    virtual WakeCause wakeCause() = 0;
    // Whether `pin` can wake the chip from deep sleep.
    virtual bool wakePin(int8_t pin) = 0;
    // Deep sleep until `us` have passed (0: no timer) or `pin` (-1: none) is
    // at the `high` level, held there by the chip's pull resistor when
    // `pull`. Returns only if the chip couldn't sleep.
    virtual void deepSleep(uint64_t us, int8_t pin, bool high, bool pull) = 0;
};

// GPIO inputs: Battery's.
class Pins {
public:
    virtual ~Pins() = default;
    // Prepares `pin` for analogMv(); false if it has no ADC that works beside
    // Wi-Fi (ADC1 on the ESP32).
    virtual bool analogBegin(int8_t pin) = 0;
    // The pin's voltage, calibrated, in millivolts.
    virtual bool analogMv(int8_t pin, uint32_t& mv) = 0;
    // Prepares `pin` as a digital input, pulled up or not.
    virtual bool inputBegin(int8_t pin, bool pullUp) = 0;
    virtual bool input(int8_t pin) = 0;
};

// What a device runs on. Null members are filled with the ESP
// implementations by the services that use them.
struct Platform {
    Core* core = nullptr;
    Wifi* wifi = nullptr;
    Ethernet* ethernet = nullptr;
    Mdns* mdns = nullptr;
    Clock* clock = nullptr;
    Ota* ota = nullptr;
    HttpClient* http = nullptr;
    Coredump* coredump = nullptr;  // stays null without a core dump partition
    Udp* udp = nullptr;
    MqttClient* mqtt = nullptr;
    Ble* ble = nullptr;
    NowTpRadio* nowtp = nullptr;
    Filesystem* fs = nullptr;
    LogOutput* log = nullptr;
    Power* power = nullptr;
    Pins* pins = nullptr;
    UdpApiTransport* udpApi = nullptr;
};

// The ESP implementations (null on the host).
Core* espCore();
Wifi* espWifi();
Ethernet* espEthernet();  // null on chips without an internal Ethernet MAC
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
Filesystem* espFilesystem();  // null if the build has no LittleFS
LogOutput* espLog();
Power* espPower();
Pins* espPins();
UdpApiTransport* espUdpApi();

}  // namespace tesserkit
