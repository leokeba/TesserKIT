// The services' client-reachable paths on a short heap (alloc_cap.h): every
// allocation a client can make the device do is reserved from TesserAPI's
// accountant, which refuses it on a short heap, or small. An allocation
// past the heap is what aborts a classic ESP32.
#pragma once

#include <stdio.h>
#include <stdlib.h>

#include "alloc_cap.h"
#include "auth_cases.h"
#include "record_cases.h"
#include "backup_cases.h"
#include "files_cases.h"
#include "firmware_cases.h"
#include "rig.h"

namespace heap_cases {

using rig::Status;

// The heap the device has above what it holds at rest. A classic ESP32 with
// every service has 25 to 45 KB free (DESIGN.md section 11); the host's
// 64-bit structures and libc++'s strings take more for the same work.
// Each case runs at 96, 32 and 12 KB (ALLOCCAP_BYTES: one size of your own).
inline std::vector<size_t> heaps() {
    const char* v = getenv("ALLOCCAP_BYTES");
    if (v && *v) return {static_cast<size_t>(atol(v))};
    return {96 * 1024, 32 * 1024, 12 * 1024};
}

// Fails the case with the allocations that went past the heap.
void checkHeap(const alloccap::Heap& h, const char* what) {
    if (getenv("ALLOCCAP_PEAKS")) printf("  %s: peak %lld bytes\n", what, h.peak());
    if (!CHECK(h.overruns().empty())) printf("%s past the heap:\n%s", what, h.report().c_str());
}

// A transfer the test drives as HTTP would, its own buffers allocated
// before the heap starts.
Status upload(rig::Rig& r, const char* path, const std::string& data, std::string_view filename = {}) {
    tesser::FileTransfer t(r.dev.api());
    tesser::Client c;
    c.authenticated = true;
    tesser::StringReply reply;
    reply.body.reserve(1024);
    if (t.openUpload(path, c, data.size(), reply, filename) != tesser::FileTransfer::Open::Ok) return reply.status;
    for (size_t off = 0; off < data.size(); off += 2048) {
        size_t n = std::min<size_t>(2048, data.size() - off);
        if (!t.write(reinterpret_cast<const uint8_t*>(data.data() + off), n, reply)) return reply.status;
    }
    t.finish(reply);
    return reply.status;
}

size_t download(rig::Rig& r, const char* path) {
    tesser::FileTransfer t(r.dev.api());
    tesser::Client c;
    c.authenticated = true;
    tesser::StringReply reply;
    reply.body.reserve(1024);
    if (t.openDownload(path, c, reply) != tesser::FileTransfer::Open::Ok) {
        if (getenv("ALLOCCAP_PEAKS")) printf("  download %s: %s\n", path, reply.body.c_str());
        return 0;
    }
    uint8_t buf[700];
    size_t total = 0;
    int n;
    while ((n = t.read(buf, sizeof(buf))) > 0) total += static_cast<size_t>(n);
    return total;
}

// The cap itself: what goes past it is caught, what was there before and
// what is given back count as they should, and TesserAPI's accountant sees
// what is left.
TEST(heap_cap_catches_an_overrun) {
    std::string before(4000, 'b');  // allocated before: freeing it gives nothing back
    {
        alloccap::Heap h(8192, 1024);
        before = std::string();
        CHECK_EQ(h.live(), 0);
        char* small = new char[4000];
        CHECK(h.live() >= 4000);
        CHECK(tesser::freeHeap() <= 8192 - 4000);
        CHECK(!tesser::reserveBytes(6000));  // refused: the floor kept
        delete[] small;
        CHECK_EQ(h.live(), 0);
        CHECK(h.overruns().empty());
        char* big = new char[10000];  // what aborts on ESP-IDF
        delete[] big;
        std::vector<alloccap::Overrun> o = h.overruns();
        CHECK_EQ(o.size(), size_t(1));
        if (!o.empty()) CHECK_EQ(o[0].size, size_t(10000));
        CHECK(h.report().find("heap_cases") != std::string::npos);  // where it came from
    }
    CHECK(tesser::freeHeap() == SIZE_MAX);  // the platform's again
}

TEST(heap_device_reads_and_writes) {
    for (size_t bytes : heaps()) {
        record_cases::FullRig r;
        CHECK(r.dev.begin());
        std::string failed;
        CHECK(record_cases::fill(r, failed));  // the largest tree there is
        r.run(2500);
        tesser::Query schema;
        schema.view = tesser::View::Schema;
        alloccap::Heap h(bytes);
        for (int i = 0; i < 3; i++) {
            r.get("/");          // the whole tree, about 20 KB
            r.get("/", schema);  // and its schema, about 32 KB: refused (busy) when it can't fit
            r.get("/sys/network/wifi/networks");
            r.set("/sys/time/tz", i % 2 ? "\"EST5EDT\"" : "\"CET-1CEST,M3.5.0,M10.5.0/3\"");
            r.run(2500);  // and saved
        }
        checkHeap(h, "reads, writes and saves");
    }
}

// The largest settings there are (record_cases.h): downloaded, restored and
// stored as a file.
TEST(heap_backup) {
    for (size_t bytes : heaps()) {
        record_cases::FullRig r;
        CHECK(r.dev.begin());
        std::string failed;
        CHECK(record_cases::fill(r, failed));
        r.run(2500);
        std::string text;
        {
            rig::Reply d = r.download("/sys/backup/settings");
            CHECK_EQ(d.status, Status::Ok);
            text = d.body;  // about 28 KB, defaults included
        }
        alloccap::Heap h(bytes);
        for (int i = 0; i < 2; i++) {
            download(r, "/sys/backup/settings");
            upload(r, "/sys/backup/settings", text);
            r.set("/sys/backup/save", "\"stored\"");
            download(r, "/sys/backup/stored/stored/content");
            r.set("/sys/backup/stored/stored/restore");
            r.run(1200);  // the restart it asks for
        }
        checkHeap(h, "backups");
    }
}

TEST(heap_files) {
    for (size_t bytes : heaps()) {
        files_cases::FilesRig r;
        CHECK(r.dev.begin());
        std::string big(100 * 1024, 'f');
        std::vector<std::string> names;
        for (int i = 0; i < 40; i++) names.push_back("file-" + std::to_string(i) + ".txt");
        alloccap::Heap h(bytes);
        CHECK_EQ(upload(r, "/sys/files/upload", big, "big.bin"), Status::Ok);
        r.step();  // listed
        CHECK_EQ(download(r, "/sys/files/entries/big.bin/content"), big.size());
        for (const std::string& n : names) upload(r, "/sys/files/upload", "x", n);
        r.step();
        r.get("/sys/files");
        r.set("/sys/backup/save", "\"one\"");
        r.set("/sys/backup/save", "\"two\"");
        checkHeap(h, "files");
    }
}

TEST(heap_firmware) {
    for (size_t bytes : heaps()) {
        firmware_cases::GithubRig r;
        r.fake.http.files[firmware_cases::kLatest] =
            firmware_cases::releaseJson("v1.2.0", {"fw-esp32c3.bin", "fw-esp32.bin"}, 20000);
        std::string img = fake::image(0, 300000);
        CHECK(r.dev.begin());
        alloccap::Heap h(bytes);
        r.check();  // a GitHub release with 20 KB of notes, filtered as it streams
        CHECK_EQ(upload(r, "/sys/firmware/image", img), Status::Ok);
        checkHeap(h, "firmware");
    }
}

TEST(heap_auth) {
    for (size_t bytes : heaps()) {
        auth_cases::AuthRig r;
        r.auth.defaultAdmin("admin", "hunter22");
        CHECK(r.dev.begin());
        std::string users = "[";
        for (int i = 0; i < 8; i++) {
            users += std::string(i ? "," : "") + "{\"name\":\"user" + std::to_string(i) +
                     "\",\"password\":\"" + std::string(64, 'p') + "\",\"role\":\"admin\"}";
        }
        users += "]";
        alloccap::Heap h(bytes);
        CHECK_EQ(r.set("/sys/auth/users", users).status, Status::Ok);
        CHECK(rig::drainBackground());
        for (int i = 0; i < 6; i++) r.login("user1", i % 2 ? "wrong" : std::string(64, 'p').c_str());
        r.run(2500);
        checkHeap(h, "accounts and sign-ins");
    }
}

}  // namespace heap_cases
