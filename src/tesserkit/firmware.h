#pragma once

#include <memory>
#include <string>

#include "tesserkit/device.h"

namespace tesserkit {

enum class FirmwareState : uint8_t { Idle, Receiving, Downloading, Verifying, Rebooting, Failed };

// What the last check of the latest release found (Firmware::github()).
enum class ReleaseState : uint8_t { Unchecked, Checking, Current, Available, Failed };

// Compares two versions in semver order ("v1.4.0", "1.4.0-rc.1"): < 0, 0 or
// > 0. A leading "v" and build metadata ("+...") are ignored; a git describe
// suffix ("-3-g1a2b3c4", "-dirty") makes a version later than its tag but
// earlier than the next. Sets `ok` to false when either isn't a version.
int compareVersions(const char* a, const char* b, bool* ok = nullptr);

// /sys/firmware: updates by upload or from a URL, confirmation and rollback
// (docs/DESIGN.md section 5.4).
class Firmware : public Service {
public:
    explicit Firmware(Device& dev) : Service(dev, "firmware") {}

    // Offers /sys/firmware/download: updates from a URL over HTTP(S), which
    // links an HTTP client and TLS (about 130 KB of flash with ESP-IDF's
    // certificate bundle). Before Device::begin().
    Firmware& downloads();
    // Offers /sys/firmware/github: updates from the latest release of a
    // GitHub repository ("owner/repo"), whose asset named `asset` is the
    // image ("{target}" stands for the chip, "esp32", and "{version}" for
    // the tag without its "v"). A URL instead of "owner/repo" names another
    // server's latest-release endpoint answering as GitHub's does (Gitea,
    // Forgejo, a test server). Implies downloads(). Before Device::begin().
    Firmware& github(const char* repo, const char* asset);
    // Whether a new device checks for a release daily on its own
    // (github/autoCheck, default true); false: only when asked, for devices
    // that run shows. After github(), before Device::begin().
    Firmware& autoCheck(bool on);

    void describe(tesser::Object& o) override;

    FirmwareState state() const { return state_; }
    bool pending() const { return pending_; }

    ReleaseState release() const;

    // A new image is confirmed once the device has been within clients'
    // reach this long (Device::reachableMs()), and retried this often if the
    // confirmation fails.
    static constexpr uint32_t kConfirmAfterMs = 60000;
    static constexpr uint32_t kConfirmRetryMs = 10000;
    // Automatic release checks: the first once the device has been online
    // this long, then one a day, or an hour after one that failed.
    static constexpr uint32_t kFirstCheckMs = 60000;
    static constexpr uint32_t kCheckEveryMs = 24u * 3600 * 1000;
    static constexpr uint32_t kRetryCheckMs = 3600u * 1000;

    // Used by transfers, from their own tasks: they take the API lock.
    void progress(uint32_t received, uint32_t total);
    void failed(const char* message);
    void verifying();
    void installed();

protected:
    void start() override;
    void tick(uint32_t nowMs) override;

private:
    Ota& ota();
    // Starts an update in `state`; false when one is already running.
    bool begin(FirmwareState state);
    void setState(FirmwareState s);
    tesser::Check download(const std::string& url);
    // A download fails after its action replied: also told as an event.
    void downloadFailed(const char* message);

    Ota* ota_ = nullptr;
    FirmwareState state_ = FirmwareState::Idle;
    int32_t progress_ = 0;
    std::string error_;
    bool pending_ = false;
    uint32_t confirmAtMs_ = 0;  // no attempt before this (after a failed one)
    tesser::Node* stateNode_ = nullptr;
    tesser::Node* progressNode_ = nullptr;
    tesser::Node* errorNode_ = nullptr;
    tesser::Node* pendingNode_ = nullptr;
    tesser::EventNode* updateFailed_ = nullptr;

    // /sys/firmware/github. Only github() creates one, so the code isn't
    // linked without it.
    class Feed {
    public:
        virtual ~Feed() = default;
        virtual void describe(tesser::Object& o) = 0;
        virtual void tick(uint32_t nowMs) = 0;
        virtual ReleaseState state() const = 0;
        virtual void autoCheck(bool on) = 0;
    };
    class GithubFeed;
    std::unique_ptr<Feed> feed_;
};

}  // namespace tesserkit
