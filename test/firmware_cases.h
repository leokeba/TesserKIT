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

TEST(firmware_image_header) {
    std::string img = fake::image(2, 64);
    const uint8_t* d = reinterpret_cast<const uint8_t*>(img.data());
    CHECK(tesserkit::checkImageHead(d, img.size(), 2).isOk());
    CHECK(!tesserkit::checkImageHead(d, img.size(), 0).isOk());
    CHECK(!tesserkit::checkImageHead(d, 35, 2).isOk());
    img[33] = 0;
    CHECK(!tesserkit::checkImageHead(reinterpret_cast<const uint8_t*>(img.data()), img.size(), 2).isOk());
}

}  // namespace firmware_cases
