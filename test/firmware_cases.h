// Firmware (docs/DESIGN.md section 5.4): uploads, downloads, confirmation,
// rollback.
#pragma once

#include "rig.h"

namespace firmware_cases {

using rig::at;
using rig::Status;
using tesserkit::FirmwareState;
using tesserkit::State;

struct FwRig : rig::Rig {
    tesserkit::System system{dev};
    tesserkit::Firmware fw{dev};
};

TEST(firmware_tree) {
    FwRig r;
    CHECK(r.dev.begin());
    std::string v = r.value("/sys/firmware");
    CHECK_EQ(at(v, "version"), "\"1.0.0\"");
    CHECK_EQ(at(v, "slot"), "\"ota_0\"");
    CHECK_EQ(at(v, "state"), "\"idle\"");
    CHECK_EQ(at(v, "pending"), "false");
    rig::Reply s = r.schema("/sys/firmware/image");
    CHECK_EQ(at(s.body, "type"), "\"file\"");
    CHECK_EQ(at(s.body, "maxSize"), "2031616");
    CHECK_EQ(at(s.body, "accept"), "\".bin\"");
    CHECK_EQ(at(s.body, "access"), "\"admin\"");
}

TEST(firmware_upload_installs_and_restarts) {
    FwRig r;
    CHECK(r.dev.begin());
    std::string img = fake::image(0, 100000);
    rig::Reply u = r.upload("/sys/firmware/image", img);
    CHECK_EQ(u.status, Status::Ok);
    CHECK(r.fake.ota.installed == img);
    CHECK_EQ(r.fw.state(), FirmwareState::Rebooting);
    CHECK_EQ(r.value("/sys/firmware/progress"), "100");
    r.step();
    CHECK_EQ(r.dev.state(), State::Updating);
    r.run(600);
    CHECK_EQ(r.fake.core.restarts, 1);
}

TEST(firmware_upload_checks_the_image_first) {
    FwRig r;
    CHECK(r.dev.begin());
    rig::Reply u = r.upload("/sys/firmware/image", fake::image(5, 100000));  // an ESP32-C3 image
    CHECK_EQ(u.status, Status::InvalidValue);
    CHECK_EQ(at(u.body, "message"), "\"image built for another chip\"");
    CHECK(r.fake.ota.written.empty());
    CHECK(r.fake.ota.installed.empty());
    CHECK_EQ(r.fw.state(), FirmwareState::Failed);
    CHECK_EQ(r.value("/sys/firmware/error"), "\"image built for another chip\"");
    r.step();
    CHECK_EQ(r.dev.state(), State::Online);

    std::string junk(5000, 'x');
    u = r.upload("/sys/firmware/image", junk);
    CHECK_EQ(u.status, Status::InvalidValue);
    CHECK_EQ(at(u.body, "message"), "\"not an app image\"");
    u = r.upload("/sys/firmware/image", "tiny");
    CHECK_EQ(u.status, Status::InvalidValue);
    CHECK_EQ(r.fake.core.restarts, 0);
}

TEST(firmware_upload_cut_short) {
    FwRig r;
    CHECK(r.dev.begin());
    rig::Reply u = r.upload("/sys/firmware/image", fake::image(0, 100000), true, 2048, 50000);
    CHECK_EQ(r.fake.ota.aborts, 1);
    CHECK(r.fake.ota.installed.empty());
    CHECK_EQ(r.fw.state(), FirmwareState::Failed);
    CHECK_EQ(r.value("/sys/firmware/error"), "\"upload interrupted\"");
    // The next one works.
    u = r.upload("/sys/firmware/image", fake::image(0, 3000));
    CHECK_EQ(u.status, Status::Ok);
}

TEST(firmware_upload_verification_fails) {
    FwRig r;
    CHECK(r.dev.begin());
    r.fake.ota.verifyFails = true;
    rig::Reply u = r.upload("/sys/firmware/image", fake::image(0, 10000));
    CHECK_EQ(u.status, Status::InvalidValue);
    CHECK_EQ(r.fw.state(), FirmwareState::Failed);
    CHECK_EQ(r.value("/sys/firmware/error"), "\"image verification failed\"");
    r.run(1000);
    CHECK_EQ(r.fake.core.restarts, 0);
}

TEST(firmware_upload_limits) {
    FwRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.upload("/sys/firmware/image", fake::image(0, 3000), false).status, Status::Unauthorized);
    r.fake.ota.slot = 0x1F0000;
    std::string big = fake::image(0, 0x1F0001);
    CHECK_EQ(r.upload("/sys/firmware/image", big).status, Status::TooLarge);
}

TEST(firmware_download) {
    FwRig r;
    std::string img = fake::image(0, 54321);
    r.fake.http.files["https://example.com/fw.bin"] = img;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/firmware/download", "\"ftp://example.com/fw.bin\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/firmware/download", "\"https://example.com/fw.bin\"").status, Status::Ok);
    CHECK_EQ(r.fw.state(), FirmwareState::Downloading);
    // One at a time.
    CHECK_EQ(r.set("/sys/firmware/download", "\"https://example.com/fw.bin\"").status, Status::Busy);
    CHECK_EQ(r.upload("/sys/firmware/image", img).status, Status::Busy);
    r.fake.core.runTasks();
    CHECK(r.fake.ota.installed == img);
    CHECK_EQ(r.fw.state(), FirmwareState::Rebooting);
    r.run(600);
    CHECK_EQ(r.fake.core.restarts, 1);
}

TEST(firmware_download_failures) {
    FwRig r;
    r.fake.http.files["http://x/wrong.bin"] = fake::image(9, 5000);
    r.fake.http.files["http://x/big.bin"] = fake::image(0, 0x200000);
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/firmware/download", "\"http://x/missing.bin\"").status, Status::Ok);
    r.fake.core.runTasks();
    CHECK_EQ(r.fw.state(), FirmwareState::Failed);
    CHECK_EQ(r.value("/sys/firmware/error"), "\"HTTP 404\"");
    r.set("/sys/firmware/download", "\"http://x/wrong.bin\"");
    r.fake.core.runTasks();
    CHECK_EQ(r.value("/sys/firmware/error"), "\"image built for another chip\"");
    r.set("/sys/firmware/download", "\"http://x/big.bin\"");
    r.fake.core.runTasks();
    CHECK_EQ(r.value("/sys/firmware/error"), "\"image larger than the slot\"");
    // Unknown length: checked as it arrives.
    r.fake.http.announceLength = false;
    r.set("/sys/firmware/download", "\"http://x/big.bin\"");
    r.fake.core.runTasks();
    CHECK_EQ(r.fw.state(), FirmwareState::Failed);
    CHECK(r.fake.ota.installed.empty());
    r.fake.core.spawnFails = true;
    CHECK_EQ(r.set("/sys/firmware/download", "\"http://x/wrong.bin\"").status, Status::Internal);
    CHECK_EQ(r.fake.core.restarts, 0);
}

TEST(firmware_confirmed_after_a_minute_online) {
    FwRig r;
    r.fake.ota.pendingImage = true;
    CHECK(r.dev.begin());
    CHECK_EQ(r.value("/sys/firmware/pending"), "true");
    r.run(59000);
    CHECK_EQ(r.fake.ota.confirms, 0);
    r.run(1500);
    CHECK_EQ(r.fake.ota.confirms, 1);
    CHECK_EQ(r.value("/sys/firmware/pending"), "false");
}

TEST(firmware_confirm_retried_after_a_failure) {
    FwRig r;
    r.fake.ota.pendingImage = true;
    r.fake.ota.confirmFails = true;
    CHECK(r.dev.begin());
    r.run(60500);
    CHECK_EQ(r.fake.ota.confirms, 1);
    CHECK_EQ(r.value("/sys/firmware/pending"), "true");
    r.fake.ota.confirmFails = false;
    r.run(5000);
    CHECK_EQ(r.fake.ota.confirms, 1);  // not every tick
    r.run(6000);
    CHECK_EQ(r.fake.ota.confirms, 2);
    CHECK_EQ(r.value("/sys/firmware/pending"), "false");
}

// A device out of its router's reach, used through its access point: the
// image is confirmed once a station has been on the access point for a
// minute, and not while nobody can reach it (DESIGN.md section 5.4).
TEST(firmware_confirmed_through_the_access_point) {
    FwRig r;
    tesserkit::Network net{r.dev};
    net.defaultNetwork("Home", "password1");  // not in range
    r.fake.ota.pendingImage = true;
    CHECK(r.dev.begin());
    r.run(120000);
    CHECK(r.fake.wifi.apUp);
    CHECK_EQ(r.dev.state(), State::Connecting);
    CHECK_EQ(r.fake.ota.confirms, 0);
    r.fake.wifi.apStations(1);
    r.run(30000);
    r.fake.wifi.apStations(0);  // the phone leaves: the minute starts again
    r.run(1000);
    r.fake.wifi.apStations(1);
    r.run(59000);
    CHECK_EQ(r.fake.ota.confirms, 0);
    r.run(1500);
    CHECK_EQ(r.fake.ota.confirms, 1);
    CHECK_EQ(r.value("/sys/firmware/pending"), "false");
}

TEST(firmware_update_confirms_the_running_image) {
    FwRig r;
    r.fake.ota.pendingImage = true;
    CHECK(r.dev.begin());
    CHECK_EQ(r.upload("/sys/firmware/image", fake::image(0, 3000)).status, Status::Ok);
    CHECK_EQ(r.fake.ota.confirms, 1);
}

TEST(firmware_confirm_and_rollback_actions) {
    FwRig r;
    r.fake.ota.pendingImage = true;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/firmware/confirm").status, Status::Ok);
    CHECK_EQ(r.fake.ota.confirms, 1);
    CHECK_EQ(r.value("/sys/firmware/pending"), "false");
    r.fake.ota.canRollBack = false;
    CHECK_EQ(r.set("/sys/firmware/rollback").status, Status::NotAllowed);
    r.fake.ota.canRollBack = true;
    CHECK_EQ(r.set("/sys/firmware/rollback").status, Status::Ok);
    CHECK_EQ(r.fake.ota.rollbacks, 1);
    r.run(600);
    CHECK_EQ(r.fake.core.restarts, 1);
}

TEST(firmware_compare_versions) {
    using tesserkit::compareVersions;
    CHECK_EQ(compareVersions("1.2.3", "1.2.3"), 0);
    CHECK_EQ(compareVersions("v1.2.3", "1.2.3"), 0);
    CHECK_EQ(compareVersions("1.2.4", "1.2.3"), 1);
    CHECK_EQ(compareVersions("1.10.0", "1.9.9"), 1);
    CHECK_EQ(compareVersions("2", "1.9"), 1);
    CHECK_EQ(compareVersions("1.2", "1.2.0"), 0);
    CHECK_EQ(compareVersions("1.2.3+build.7", "1.2.3"), 0);
    // Prereleases come before their release, in semver's order.
    CHECK_EQ(compareVersions("1.2.3-rc.1", "1.2.3"), -1);
    CHECK_EQ(compareVersions("1.2.3-rc.2", "1.2.3-rc.10"), -1);
    CHECK_EQ(compareVersions("1.2.3-alpha", "1.2.3-alpha.1"), -1);
    CHECK_EQ(compareVersions("1.2.3-1", "1.2.3-alpha"), -1);
    CHECK_EQ(compareVersions("1.2.3-beta", "1.2.3-alpha"), 1);
    // git describe: after the tag, before the next release.
    CHECK_EQ(compareVersions("v1.2.3-4-g1a2b3c4", "1.2.3"), 1);
    CHECK_EQ(compareVersions("v1.2.3-4-g1a2b3c4-dirty", "1.2.4"), -1);
    CHECK_EQ(compareVersions("v1.2.3-dirty", "v1.2.3"), 1);
    CHECK_EQ(compareVersions("1.2.3-rc.1-2-gabcdef0", "1.2.3"), -1);
    // Not versions.
    bool ok = true;
    compareVersions("1e79a96-dirty", "1.0.0", &ok);
    CHECK(!ok);
    compareVersions("1.0.0", "", &ok);
    CHECK(!ok);
    compareVersions("1.2.3.4", "1.0.0", &ok);
    CHECK(!ok);
    compareVersions("1.", "1.0.0", &ok);
    CHECK(!ok);
    compareVersions("v0.3.1", "0.3.1-2-g0e79a96", &ok);
    CHECK(ok);
}

// GitHub's answer for a release, with the fields a filter must skip.
inline std::string releaseJson(const std::string& tag, const std::vector<std::string>& assets,
                               size_t notes = 200) {
    JsonDocument d;
    d["url"] = "https://api.github.com/repos/o/r/releases/1";
    d["author"]["login"] = "github-actions[bot]";
    d["tag_name"] = tag;
    d["name"] = tag;
    d["prerelease"] = false;
    for (const std::string& a : assets) {
        JsonObject o = d["assets"].add<JsonObject>();
        o["url"] = "https://api.github.com/repos/o/r/releases/assets/1";
        o["name"] = a;
        o["uploader"]["login"] = "github-actions[bot]";
        o["size"] = 1000000;
        o["browser_download_url"] = "https://github.com/o/r/releases/download/" + tag + "/" + a;
    }
    d["body"] = std::string(notes, 'n');
    std::string out;
    serializeJson(d, out);
    return out;
}

constexpr const char* kLatest = "https://api.github.com/repos/o/r/releases/latest";

struct GithubRig : FwRig {
    explicit GithubRig(const char* repo = "o/r", const char* asset = "fw-{target}.bin") { fw.github(repo, asset); }
    // Asks for a check: the request, the check's task, the reply.
    rig::Reply check() {
        auto out = std::make_shared<rig::Reply>();
        admin.request(rig::Op::Set, "/sys/firmware/github/check", {}, [out](Status s, JsonVariantConst b) {
            out->status = s;
            serializeJson(b, out->body);
            out->done = true;
        });
        admin.process();
        fake.core.runTasks();
        step();
        admin.process();
        return *out;
    }
    // Hours pass, in steps of 10 s.
    void skip(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10000) step(10000);
    }
};

TEST(firmware_github_tree) {
    {
        FwRig r;
        CHECK(r.dev.begin());
        CHECK_EQ(r.get("/sys/firmware/github").status, Status::NotFound);
    }
    GithubRig r;
    CHECK(r.dev.begin());
    std::string v = r.value("/sys/firmware/github");
    CHECK_EQ(at(v, "repo"), "\"o/r\"");
    CHECK_EQ(at(v, "latest"), "\"\"");
    CHECK_EQ(at(v, "status"), "\"unchecked\"");
    CHECK_EQ(at(v, "autoCheck"), "true");
    CHECK_EQ(at(r.schema("/sys/firmware/github/update").body, "access"), "\"admin\"");
    CHECK_EQ(at(r.schema("/sys/firmware/github/check").body, "deferred"), "true");
    // github() implies downloads().
    CHECK_EQ(r.schema("/sys/firmware/download").status, Status::Ok);
}

TEST(firmware_github_check_and_update) {
    GithubRig r;
    r.fake.http.files[kLatest] = releaseJson("v1.2.0", {"fw-esp32c3.bin", "fw-esp32.bin", "fw-esp32-factory.bin"});
    std::string img = fake::image(0, 40000);
    r.fake.http.files["https://github.com/o/r/releases/download/v1.2.0/fw-esp32.bin"] = img;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/firmware/github/update").status, Status::NotAllowed);
    rig::Reply c = r.check();
    CHECK(c.done);
    CHECK_EQ(c.status, Status::Ok);
    CHECK_EQ(c.body, "\"v1.2.0\"");
    CHECK_EQ(r.fake.http.requests.size(), 1u);
    CHECK_EQ(r.fw.release(), tesserkit::ReleaseState::Available);
    CHECK_EQ(r.value("/sys/firmware/github/latest"), "\"v1.2.0\"");
    CHECK_EQ(r.value("/sys/firmware/github/status"), "\"available\"");
    CHECK_EQ(r.as(tesser::Access::User, rig::Op::Set, "/sys/firmware/github/update").status, Status::Unauthorized);
    CHECK_EQ(r.set("/sys/firmware/github/update").status, Status::Ok);
    CHECK_EQ(r.fw.state(), FirmwareState::Downloading);
    r.fake.core.runTasks();
    CHECK(r.fake.ota.installed == img);
    CHECK_EQ(r.fw.state(), FirmwareState::Rebooting);
    r.run(600);
    CHECK_EQ(r.fake.core.restarts, 1);
}

TEST(firmware_https_waits_for_memory) {
    GithubRig r;
    r.fake.http.files[kLatest] = releaseJson("v1.2.0", {"fw-esp32.bin"});
    r.fake.http.files["https://example.com/fw.bin"] = fake::image(0, 54321);
    CHECK(r.dev.begin());
    // Too little heap for a TLS handshake: refused before anything starts,
    // rather than leaving the firmware without memory.
    r.fake.core.heap = 30000;
    rig::Reply c = r.check();
    CHECK_EQ(c.status, Status::Busy);
    CHECK(r.fake.http.requests.empty());
    CHECK_EQ(r.value("/sys/firmware/github/status"), "\"failed\"");
    CHECK(r.value("/sys/firmware/github/error").find("memory") != std::string::npos);
    CHECK_EQ(r.set("/sys/firmware/download", "\"https://example.com/fw.bin\"").status, Status::Busy);
    CHECK_EQ(r.fw.state(), FirmwareState::Idle);
    // Plain HTTP needs no handshake, but its task and buffers.
    r.fake.http.files["http://192.168.1.3/fw.bin"] = fake::image(0, 54321);
    r.fake.core.heap = 20000;
    CHECK_EQ(r.set("/sys/firmware/download", "\"http://192.168.1.3/fw.bin\"").status, Status::Busy);
    CHECK_EQ(r.fw.state(), FirmwareState::Idle);
    r.fake.core.heap = 30000;
    CHECK_EQ(r.set("/sys/firmware/download", "\"http://192.168.1.3/fw.bin\"").status, Status::Ok);
}

TEST(firmware_https_once_memory_is_back) {
    GithubRig r;
    r.fake.http.files[kLatest] = releaseJson("v1.2.0", {"fw-esp32.bin"});
    CHECK(r.dev.begin());
    r.fake.core.heap = 30000;
    CHECK_EQ(r.check().status, Status::Busy);
    r.fake.core.heap = 180000;
    CHECK_EQ(r.check().status, Status::Ok);
    CHECK_EQ(r.value("/sys/firmware/github/error"), "\"\"");
}

TEST(firmware_github_up_to_date) {
    GithubRig r;
    r.fake.http.files[kLatest] = releaseJson("v1.0.0", {"fw-esp32.bin"});
    CHECK(r.dev.begin());
    CHECK_EQ(r.check().body, "\"v1.0.0\"");
    CHECK_EQ(r.value("/sys/firmware/github/status"), "\"current\"");
    CHECK_EQ(r.set("/sys/firmware/github/update").status, Status::NotAllowed);
    // An older release isn't offered either.
    r.fake.http.files[kLatest] = releaseJson("0.9.9", {"fw-esp32.bin"});
    r.check();
    CHECK_EQ(r.fw.release(), tesserkit::ReleaseState::Current);
    // A running version that isn't semver: any other release is offered.
    r.dev.version("e79a96f-dirty");
    r.check();
    CHECK_EQ(r.fw.release(), tesserkit::ReleaseState::Available);
}

TEST(firmware_github_check_failures) {
    GithubRig r;
    CHECK(r.dev.begin());
    rig::Reply c = r.check();  // no release at all
    CHECK_EQ(c.status, Status::NotFound);
    CHECK_EQ(at(c.body, "message"), "\"no release found\"");
    CHECK_EQ(r.value("/sys/firmware/github/status"), "\"failed\"");
    CHECK_EQ(r.value("/sys/firmware/github/error"), "\"no release found\"");

    r.fake.http.files[kLatest] = releaseJson("v2.0.0", {"fw-esp32s3.bin"});
    r.fake.http.statuses[kLatest] = 403;
    CHECK_EQ(at(r.check().body, "message"), "\"GitHub's rate limit: try again later\"");
    r.fake.http.statuses.clear();

    // The release lacks this chip's image: shown, not offered.
    c = r.check();
    CHECK_EQ(c.status, Status::NotFound);
    CHECK_EQ(at(c.body, "message"), "\"v2.0.0 has no fw-esp32.bin\"");
    CHECK_EQ(r.value("/sys/firmware/github/latest"), "\"v2.0.0\"");
    CHECK_EQ(r.set("/sys/firmware/github/update").status, Status::NotAllowed);

    r.fake.http.files[kLatest] = "<html>";
    CHECK_EQ(at(r.check().body, "message"), "\"unreadable answer\"");
    r.fake.http.files[kLatest] = releaseJson("v2.0.0", {"fw-esp32.bin"});
    r.fake.http.cutAt = 100;
    CHECK_EQ(at(r.check().body, "message"), "\"answer cut short\"");
    r.fake.http.cutAt = 0;
    r.fake.http.unreachable = true;
    CHECK_EQ(at(r.check().body, "message"), "\"server unreachable\"");
    r.fake.http.unreachable = false;
    r.fake.core.spawnFails = true;
    CHECK_EQ(at(r.check().body, "message"), "\"no memory for the check\"");
    r.fake.core.spawnFails = false;

    // Recovers, and a success clears the error.
    c = r.check();
    CHECK_EQ(c.status, Status::Ok);
    CHECK_EQ(r.value("/sys/firmware/github/error"), "\"\"");
    CHECK_EQ(r.fw.release(), tesserkit::ReleaseState::Available);
}

TEST(firmware_github_large_release) {
    // WLED's latest release is 72 KB of JSON with 33 assets: streamed
    // through a filter, never held whole.
    GithubRig r("o/r", "fw_{version}_{target}.bin");
    std::vector<std::string> assets;
    for (int i = 0; i < 40; i++) assets.push_back("fw_3.1.4_board" + std::to_string(i) + ".bin");
    assets.push_back("fw_3.1.4_esp32.bin");
    r.fake.http.files[kLatest] = releaseJson("v3.1.4", assets, 60000);
    r.fake.http.announceLength = false;
    CHECK(r.dev.begin());
    CHECK_EQ(r.check().status, Status::Ok);
    CHECK_EQ(r.fw.release(), tesserkit::ReleaseState::Available);
    r.fake.http.files["https://github.com/o/r/releases/download/v3.1.4/fw_3.1.4_esp32.bin"] = fake::image(0, 3000);
    CHECK_EQ(r.set("/sys/firmware/github/update").status, Status::Ok);
    r.fake.core.runTasks();
    CHECK_EQ(r.fw.state(), FirmwareState::Rebooting);
}

TEST(firmware_github_other_servers) {
    // A URL names the latest-release endpoint itself (Gitea, a test server).
    GithubRig r("http://192.168.1.10:8000/latest", "fw.bin");
    r.fake.http.files["http://192.168.1.10:8000/latest"] = releaseJson("v1.0.1", {"fw.bin"});
    CHECK(r.dev.begin());
    CHECK_EQ(r.check().status, Status::Ok);
    CHECK_EQ(r.fw.release(), tesserkit::ReleaseState::Available);
}

TEST(firmware_github_checks_by_itself) {
    GithubRig r;
    r.fake.http.files[kLatest] = releaseJson("v1.0.0", {"fw-esp32.bin"});
    CHECK(r.dev.begin());
    // The first check once online for a minute.
    r.run(59000);
    CHECK(r.fake.core.tasks.empty());
    r.run(1100);
    CHECK_EQ(r.fake.core.tasks.size(), 1u);
    CHECK_EQ(r.fw.release(), tesserkit::ReleaseState::Checking);
    r.run(1000);  // one check at a time
    CHECK_EQ(r.fake.core.tasks.size(), 1u);
    r.fake.core.runTasks();
    CHECK_EQ(r.fw.release(), tesserkit::ReleaseState::Current);
    // Then daily.
    r.skip(tesserkit::Firmware::kRetryCheckMs);
    CHECK(r.fake.core.tasks.empty());
    r.skip(tesserkit::Firmware::kCheckEveryMs - tesserkit::Firmware::kRetryCheckMs);
    CHECK_EQ(r.fake.core.tasks.size(), 1u);
    // A failure is retried an hour later.
    r.fake.http.files.clear();
    r.fake.core.runTasks();
    CHECK_EQ(r.fw.release(), tesserkit::ReleaseState::Failed);
    r.skip(tesserkit::Firmware::kRetryCheckMs - 10000);
    CHECK(r.fake.core.tasks.empty());
    r.skip(10000);
    CHECK_EQ(r.fake.core.tasks.size(), 1u);
    r.fake.core.runTasks();
    // Never when turned off, and no check during an update.
    CHECK_EQ(r.set("/sys/firmware/github/autoCheck", "false").status, Status::Ok);
    r.skip(tesserkit::Firmware::kCheckEveryMs);
    CHECK(r.fake.core.tasks.empty());
    CHECK_EQ(r.set("/sys/firmware/download", "\"https://x/fw.bin\"").status, Status::Ok);
    CHECK_EQ(r.check().status, Status::Busy);
}

TEST(firmware_image_header) {
    std::string img = fake::image(2, 64);
    const uint8_t* d = reinterpret_cast<const uint8_t*>(img.data());
    CHECK(tesserkit::checkImageHead(d, img.size(), 2).isOk());
    CHECK(!tesserkit::checkImageHead(d, img.size(), 0).isOk());
    CHECK(!tesserkit::checkImageHead(d, 35, 2).isOk());
    img[33] = 0;
    CHECK(!tesserkit::checkImageHead(reinterpret_cast<const uint8_t*>(img.data()), img.size(), 2).isOk());
}

TEST(firmware_failed_downloads_are_events) {
    FwRig r;
    CHECK(r.dev.begin());
    rig::Events ev(r, "/sys/firmware/updateFailed");
    std::string s = r.schema("/sys/firmware/updateFailed").body;
    CHECK_EQ(at(s, "ui/toast"), "\"error\"");
    CHECK_EQ(at(s, "ui/hidden"), "true");
    // A download fails after its action replied.
    CHECK_EQ(r.set("/sys/firmware/download", "\"http://x/missing.bin\"").status, Status::Ok);
    r.fake.core.runTasks();
    std::vector<std::string> got = ev.take();
    CHECK_EQ(got.size(), size_t(1));
    if (!got.empty()) CHECK_EQ(got[0], "\"HTTP 404\"");
    // An upload's error is in its own reply.
    CHECK_EQ(r.upload("/sys/firmware/image", fake::image(9, 5000)).status, Status::InvalidValue);
    CHECK(ev.take().empty());
}

TEST(firmware_github_new_releases_are_events) {
    GithubRig r;
    r.fake.http.files[kLatest] = releaseJson("v1.2.0", {"fw-esp32.bin"});
    CHECK(r.dev.begin());
    rig::Events ev(r, "/sys/firmware/github/newRelease");
    CHECK_EQ(at(r.schema("/sys/firmware/github/newRelease").body, "ui/toast"), "\"info\"");
    // Found by the automatic check: told once.
    r.run(60100);
    r.fake.core.runTasks();
    std::vector<std::string> got = ev.take();
    CHECK_EQ(got.size(), size_t(1));
    if (!got.empty()) CHECK_EQ(got[0], "\"v1.2.0\"");
    r.skip(tesserkit::Firmware::kCheckEveryMs);
    r.fake.core.runTasks();
    CHECK_EQ(r.fw.release(), tesserkit::ReleaseState::Available);
    CHECK(ev.take().empty());
    // A client that asks has the answer in its reply.
    r.fake.http.files[kLatest] = releaseJson("v1.3.0", {"fw-esp32.bin"});
    CHECK_EQ(r.check().body, "\"v1.3.0\"");
    CHECK(ev.take().empty());
    r.fake.http.files[kLatest] = releaseJson("v1.4.0", {"fw-esp32.bin"});
    r.skip(tesserkit::Firmware::kCheckEveryMs);
    r.fake.core.runTasks();
    got = ev.take();
    CHECK_EQ(got.size(), size_t(1));
    if (!got.empty()) CHECK_EQ(got[0], "\"v1.4.0\"");
}

}  // namespace firmware_cases
