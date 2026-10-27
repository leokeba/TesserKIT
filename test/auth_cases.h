// Auth (docs/DESIGN.md section 5.7) and its crypto.
#pragma once

#include "rig.h"
#include "tesserkit/crypto.h"

namespace auth_cases {

using rig::at;
using rig::Op;
using rig::Status;
using tesser::Access;
namespace crypto = tesserkit::crypto;

std::string hex(const uint8_t* d, size_t n) { return crypto::toHex(d, n); }

TEST(crypto_sha256_vectors) {
    uint8_t h[32];
    crypto::sha256("abc", 3, h);
    CHECK_EQ(hex(h, 32), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    crypto::sha256("", 0, h);
    CHECK_EQ(hex(h, 32), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    crypto::sha256(two, strlen(two), h);
    CHECK_EQ(hex(h, 32), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    std::string million(1000000, 'a');
    crypto::Sha256 s;
    for (size_t i = 0; i < million.size(); i += 777) s.update(million.data() + i, std::min<size_t>(777, million.size() - i));
    s.finish(h);
    CHECK_EQ(hex(h, 32), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(crypto_hmac_and_pbkdf2_vectors) {
    uint8_t h[32];
    uint8_t key[20];
    memset(key, 0x0b, sizeof(key));
    crypto::hmacSha256(key, sizeof(key), "Hi There", 8, h);  // RFC 4231, case 1
    CHECK_EQ(hex(h, 32), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    crypto::hmacSha256("Jefe", 4, "what do ya want for nothing?", 28, h);  // case 2
    CHECK_EQ(hex(h, 32), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    uint8_t big[131];
    memset(big, 0xaa, sizeof(big));  // case 6: a key longer than a block
    const char* m = "Test Using Larger Than Block-Size Key - Hash Key First";
    crypto::hmacSha256(big, sizeof(big), m, strlen(m), h);
    CHECK_EQ(hex(h, 32), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    const uint8_t salt[] = {'s', 'a', 'l', 't'};
    crypto::pbkdf2Sha256("password", salt, 4, 1, h, 32);
    CHECK_EQ(hex(h, 32), "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    crypto::pbkdf2Sha256("password", salt, 4, 4096, h, 32);
    CHECK_EQ(hex(h, 32), "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    uint8_t long40[40];
    const uint8_t salt2[] = "saltSALTsaltSALTsaltSALTsaltSALTsalt";
    crypto::pbkdf2Sha256("passwordPASSWORDpassword", salt2, 36, 4096, long40, 40);
    CHECK_EQ(hex(long40, 40), "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9");
    uint8_t back[3];
    CHECK(crypto::fromHex("00ff7A", back, 3));
    CHECK_EQ(hex(back, 3), "00ff7a");
    CHECK(!crypto::fromHex("00ff7", back, 3));
    CHECK(!crypto::fromHex("00fg7a", back, 3));
    CHECK(crypto::equal("abc", "abc"));
    CHECK(!crypto::equal("abc", "abd"));
    CHECK(!crypto::equal("abc", "ab"));
}

struct AuthRig : rig::Rig {
    tesserkit::System system{dev};
    tesserkit::Time time{dev};
    tesserkit::Auth auth{dev};
    // Signs in, and waits for the background task's answer.
    rig::Reply loginReply(const char* name, const char* password, bool* deferred = nullptr) {
        std::string body = std::string("{\"name\":\"") + name + "\",\"password\":\"" + password + "\"}";
        auto r = std::make_shared<rig::Reply>();
        anon.request(Op::Set, "/sys/auth/login", body, [r](Status s, JsonVariantConst b) {
            r->status = s;
            r->body.clear();
            serializeJson(b, r->body);
            r->done = true;
        });
        anon.process();
        if (deferred) *deferred = !r->done;
        CHECK(rig::drainBackground());
        anon.process();
        return *r;
    }
    std::string login(const char* name, const char* password, Status* status = nullptr) {
        rig::Reply r = loginReply(name, password);
        if (status) *status = r.status;
        return r.status == Status::Ok ? at(r.body, "token").substr(1, at(r.body, "token").size() - 2) : "";
    }
};

TEST(auth_open_until_an_admin_exists) {
    AuthRig r;
    CHECK(r.dev.begin());
    CHECK(r.auth.open());
    CHECK_EQ(r.value("/sys/auth/open"), "true");
    CHECK_EQ(r.dev.api().tokenAccess(""), Access::Admin);
    // A user account doesn't close it.
    CHECK_EQ(r.set("/sys/auth/users", "[{\"name\":\"bob\",\"password\":\"bobpass\"}]").status, Status::Ok);
    CHECK(r.auth.open());
    CHECK_EQ(r.set("/sys/auth/users/bob/role", "\"admin\"").status, Status::Ok);
    CHECK(!r.auth.open());
    CHECK_EQ(r.dev.api().tokenAccess(""), Access::Public);
    r.step();
    CHECK_EQ(r.value("/sys/auth/open"), "false");
}

TEST(auth_passwords_are_hashed) {
    AuthRig r;
    CHECK(r.dev.begin());
    r.set("/sys/auth/users", "[{\"name\":\"ann\",\"password\":\"s3cret\",\"role\":\"admin\"}]");
    CHECK_EQ(r.value("/sys/auth/users/ann/password"), "null");  // never readable
    CHECK(rig::drainBackground());  // the hash
    r.run(2500);
    const std::string& saved = r.storage.records["sys"];
    CHECK(saved.find("s3cret") == std::string::npos);
    CHECK(saved.find("$pbkdf2-sha256$2000$") != std::string::npos);
    CHECK_EQ(r.set("/sys/auth/users/ann/password", "\"\"").status, Status::InvalidValue);
}

TEST(auth_login_and_tokens) {
    AuthRig r;
    r.fake.clock.epoch = 1782907200000;  // the clock is set
    r.auth.defaultAdmin("admin", "hunter22");
    CHECK(r.dev.begin());
    CHECK(!r.auth.open());
    r.set("/sys/auth/users", "[{\"name\":\"admin\"},{\"name\":\"guest\",\"password\":\"guestpw\"}]");

    Status s;
    std::string admin = r.login("admin", "hunter22", &s);
    CHECK_EQ(s, Status::Ok);
    CHECK(admin.find("admin.1783511999") == std::string::npos);  // expiry: now + 168 h
    CHECK(admin.compare(0, 17, "admin.1783512000.") == 0);
    CHECK_EQ(r.dev.api().tokenAccess(admin), Access::Admin);
    std::string guest = r.login("guest", "guestpw");
    CHECK_EQ(r.dev.api().tokenAccess(guest), Access::User);

    // What each level may do.
    CHECK_EQ(r.asToken("", Op::Set, "/sys/restart").status, Status::Unauthorized);
    CHECK_EQ(r.asToken(guest, Op::Set, "/sys/restart").status, Status::Unauthorized);
    CHECK_EQ(r.asToken(admin, Op::Set, "/sys/restart").status, Status::Ok);
    CHECK_EQ(r.asToken(guest, Op::Get, "/sys/auth/users").status, Status::Unauthorized);
    CHECK_EQ(at(r.asToken("", Op::Get, "/sys/auth").body, "users"), "<missing>");
    CHECK_EQ(r.asToken("", Op::Get, "/sys/status/heap").status, Status::Ok);

    // Tampered or unknown tokens.
    std::string forged = guest;
    forged.replace(0, 5, "admin");
    CHECK_EQ(r.dev.api().tokenAccess(forged), Access::Public);
    CHECK_EQ(r.dev.api().tokenAccess("garbage"), Access::Public);
    CHECK_EQ(r.dev.api().tokenAccess("a.b.c"), Access::Public);

    // Expiry.
    r.fake.clock.epoch += 169LL * 3600 * 1000;
    CHECK_EQ(r.dev.api().tokenAccess(admin), Access::Public);
    r.fake.clock.epoch -= 169LL * 3600 * 1000;

    // A removed account's tokens stop working.
    r.set("/sys/auth/users", "[{\"name\":\"admin\"}]");
    CHECK_EQ(r.dev.api().tokenAccess(guest), Access::Public);
    // logoutAll revokes every token.
    CHECK_EQ(r.dev.api().tokenAccess(admin), Access::Admin);
    CHECK_EQ(r.set("/sys/auth/logoutAll").status, Status::Ok);
    CHECK_EQ(r.dev.api().tokenAccess(admin), Access::Public);
}

TEST(auth_tokens_survive_restarts) {
    std::map<std::string, std::string> saved;
    std::string token;
    {
        AuthRig r;
        r.fake.clock.epoch = 1782907200000;  // a clock: the token's expiry is a date
        r.auth.defaultAdmin("admin", "pw");
        CHECK(r.dev.begin());
        token = r.login("admin", "pw");
        r.set("/sys/auth/users/admin/password", "\"changed\"");
        CHECK(rig::drainBackground());
        r.run(2500);
        saved = r.storage.records;
    }
    // The same firmware: the same default account, whose changed password
    // was stored.
    AuthRig r;
    r.fake.clock.epoch = 1782907200000;
    r.auth.defaultAdmin("admin", "pw");
    r.storage.records = saved;
    CHECK(r.dev.begin());
    CHECK_EQ(r.dev.api().tokenAccess(token), Access::Admin);
    CHECK(!r.auth.open());
    CHECK(r.login("admin", "pw").empty());  // the stored account, not the default
    CHECK_EQ(r.login("admin", "changed").empty(), false);
}

TEST(auth_tokens_without_a_clock_end_with_the_boot) {
    // A show network without SNTP: a token is valid for this boot, and for
    // sessionHours of its uptime, no longer forever.
    std::map<std::string, std::string> saved;
    std::string token;
    {
        AuthRig r;
        r.auth.defaultAdmin("admin", "pw");
        CHECK(r.dev.begin());
        token = r.login("admin", "pw");
        size_t b = token.find(".b");
        CHECK(b != std::string::npos);  // ".b<boot id, 8 hex digits>:<deadline>."
        CHECK_EQ(token[b + 10], ':');
        CHECK_EQ(r.dev.api().tokenAccess(token), Access::Admin);
        r.fake.core.now += 169ULL * 3600 * 1000;  // past 168 h of uptime
        CHECK_EQ(r.dev.api().tokenAccess(token), Access::Public);
        r.fake.core.now -= 169ULL * 3600 * 1000;
        CHECK_EQ(r.dev.api().tokenAccess(token), Access::Admin);
        // A token made before this change, which never expired.
        std::string old = "admin.0." + token.substr(token.rfind('.') + 1);
        CHECK_EQ(r.dev.api().tokenAccess(old), Access::Public);
        r.run(2500);
        saved = r.storage.records;
    }
    // The boot's id isn't stored: a boot doesn't rewrite /sys.
    CHECK(saved["sys"].find("boot") == std::string::npos);
    AuthRig r;
    r.fake.core.seed = 99;  // the chip's random numbers differ from boot to boot
    r.auth.defaultAdmin("admin", "pw");
    r.storage.records = saved;
    CHECK(r.dev.begin());
    int saves = r.storage.saves;
    r.run(2500);
    CHECK_EQ(r.storage.saves, saves);  // a boot saves nothing
    CHECK_EQ(r.dev.api().tokenAccess(token), Access::Public);  // another boot
    std::string again = r.login("admin", "pw");
    CHECK_EQ(r.dev.api().tokenAccess(again), Access::Admin);
    CHECK(again.substr(again.find(".b"), 10) != token.substr(token.find(".b"), 10));
}

// Settings version 3 dropped the stored boot count (/sys/auth/boots): a
// record stored before it loads without it, and isn't rewritten at each
// boot any more.
TEST(auth_boot_count_dropped_by_migration) {
    AuthRig r;
    r.storage.records["@version"] = "2";
    r.storage.records["sys"] = R"({"auth":{"boots":41,"sessionHours":24}})";
    CHECK(r.dev.begin());
    CHECK_EQ(r.value("/sys/auth/sessionHours"), "24");
    CHECK_EQ(r.storage.records["sys"], R"({"auth":{"sessionHours":24}})");
    CHECK_EQ(r.storage.records["@version"], std::to_string(tesserkit::Device::kSettingsVersion));
}

// A reply that can be detached (a deferred action, as a sign-in is),
// whatever client the request comes from.
struct DetachedReply : tesser::StringReply {
    std::shared_ptr<rig::Reply> out;
    explicit DetachedReply(std::shared_ptr<rig::Reply> o) : out(std::move(o)) {}
    void end() override {
        tesser::StringReply::end();
        out->status = status;
        out->body = body;
        out->done = true;
    }
    tesser::Reply* detach() override { return new DetachedReply(out); }
    void release() override { delete this; }
};

TEST(auth_revocations_recheck_open_sessions) {
    // TesserAPI's tokensChanged() on every change of who may do what, so a
    // WebSocket opened with a token, or while the device was open, doesn't
    // keep a level it lost.
    AuthRig r;
    CHECK(r.dev.begin());
    r.run(40);
    uint32_t g = r.dev.api().tokenGeneration();
    r.set("/sys/auth/users", R"([{"name":"ada","password":"secret-pass","role":"admin"}])");  // the device closes
    r.run(40);
    CHECK(r.dev.api().tokenGeneration() != g);
    g = r.dev.api().tokenGeneration();
    r.set("/sys/auth/users/ada/role", "\"user\"");
    r.run(40);
    CHECK(r.dev.api().tokenGeneration() != g);
    g = r.dev.api().tokenGeneration();
    CHECK_EQ(r.set("/sys/auth/logoutAll").status, Status::Ok);
    r.run(40);
    CHECK(r.dev.api().tokenGeneration() != g);
    g = r.dev.api().tokenGeneration();
    r.run(200);
    CHECK_EQ(r.dev.api().tokenGeneration(), g);  // nothing changed: nothing asked again
}

TEST(auth_backups_leave_out_the_key) {
    // A backup holding /sys/auth/key could forge tokens for this device;
    // it stays in its own storage.
    AuthRig r;
    r.auth.defaultAdmin("admin", "pw");
    CHECK(r.dev.begin());
    r.run(100);
    std::string backup = r.dev.api().persistedState();
    CHECK(backup.find("\"users\"") != std::string::npos);
    CHECK(backup.find("\"key\"") == std::string::npos);
    CHECK(backup.find("\"boots\"") == std::string::npos);
}

TEST(auth_lockout_is_per_client) {
    // Five bad passwords lock that client out for 30 s, not the others: one
    // stranger can't keep the admin out.
    AuthRig r;
    r.auth.defaultAdmin("admin", "hunter22");
    CHECK(r.dev.begin());
    auto loginFrom = [&r](uint8_t host, const char* password) {
        tesser::Client c;
        c.transport = tesser::TransportKind::Http;
        c.address[0] = 192;
        c.address[1] = 168;
        c.address[2] = 1;
        c.address[3] = host;
        c.addressLength = 4;
        std::string body = std::string("{\"name\":\"admin\",\"password\":\"") + password + "\"}";
        JsonDocument doc;
        deserializeJson(doc, body);
        tesser::Request req;
        req.op = Op::Set;
        req.path = "/sys/auth/login";
        req.body = doc.as<JsonVariantConst>();
        req.client = c;
        auto out = std::make_shared<rig::Reply>();
        DetachedReply reply(out);
        r.dev.api().handle(req, reply);
        CHECK(rig::drainBackground());
        r.run(20);
        return out->status;
    };
    for (int i = 0; i < tesserkit::Auth::kMaxFailures; i++) CHECK_EQ(loginFrom(66, "wrong"), Status::Unauthorized);
    CHECK_EQ(loginFrom(66, "hunter22"), Status::Busy);  // locked out
    CHECK_EQ(loginFrom(10, "hunter22"), Status::Ok);    // the admin, from elsewhere
    r.fake.core.now += tesserkit::Auth::kLockoutMs + 1;
    CHECK_EQ(loginFrom(66, "hunter22"), Status::Ok);
}

TEST(auth_brute_force_delay) {
    AuthRig r;
    r.auth.defaultAdmin("admin", "right");
    CHECK(r.dev.begin());
    Status s;
    for (int i = 0; i < 5; i++) {
        r.login("admin", "wrong", &s);
        CHECK_EQ(s, Status::Unauthorized);
    }
    r.login("admin", "right", &s);
    CHECK_EQ(s, Status::Busy);
    r.run(29000);
    r.login("admin", "right", &s);
    CHECK_EQ(s, Status::Busy);
    r.run(1100);
    r.login("admin", "right", &s);
    CHECK_EQ(s, Status::Ok);
    r.login("nobody", "x", &s);
    CHECK_EQ(s, Status::Unauthorized);
}

// Holds TesserAPI's background task until release(), so the sign-ins queued
// behind it wait, deterministically.
struct Gate {
    std::shared_ptr<std::atomic<bool>> open = std::make_shared<std::atomic<bool>>(false);
    Gate() {
        auto o = open;
        CHECK(tesser::background([o] {
            for (int i = 0; i < 5000 && !*o; i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }));
    }
    void release() { *open = true; }
    ~Gate() { release(); }
};

TEST(auth_login_is_answered_off_the_lock) {
    AuthRig r;
    r.fake.clock.epoch = 1782907200000;
    r.auth.defaultAdmin("admin", "hunter22");
    CHECK(r.dev.begin());
    auto send = [&r](const char* name, const char* password) {
        auto out = std::make_shared<rig::Reply>();
        std::string body = std::string("{\"name\":\"") + name + "\",\"password\":\"" + password + "\"}";
        r.anon.request(Op::Set, "/sys/auth/login", body, [out](Status s, JsonVariantConst b) {
            out->status = s;
            serializeJson(b, out->body);
            out->done = true;
        });
        r.anon.process();
        return out;
    };
    {
        Gate gate;
        auto wrong = send("admin", "nope");
        auto nobody = send("nobody", "x");
        auto right = send("admin", "hunter22");  // checked last: failures start again from 0
        // Deferred: the API answers others meanwhile.
        CHECK(!right->done && !wrong->done && !nobody->done);
        CHECK_EQ(r.get("/sys/auth/open").body, "false");
        gate.release();
        CHECK(rig::drainBackground());
        r.anon.process();
        CHECK_EQ(right->status, Status::Ok);
        CHECK_EQ(at(right->body, "access"), "\"admin\"");
        std::string token = at(right->body, "token");
        CHECK_EQ(r.dev.api().tokenAccess(token.substr(1, token.size() - 2)), Access::Admin);
        CHECK_EQ(wrong->status, Status::Unauthorized);
        CHECK_EQ(nobody->status, Status::Unauthorized);
    }
    {
        // Sign-ins being checked count toward the lockout: no more than five
        // are tried before it.
        r.dev.api().config().maxPending = 8;
        Gate gate;
        std::vector<std::shared_ptr<rig::Reply>> tries;
        for (int i = 0; i < 5; i++) tries.push_back(send("admin", "wrong"));
        auto sixth = send("admin", "hunter22");
        CHECK(sixth->done);
        CHECK_EQ(sixth->status, Status::Busy);
        gate.release();
        CHECK(rig::drainBackground());
        r.anon.process();
        for (auto& t : tries) CHECK_EQ(t->status, Status::Unauthorized);
        Status s;
        r.login("admin", "hunter22", &s);
        CHECK_EQ(s, Status::Busy);  // locked out
    }
    {
        // An account changed while its password was being checked: refused.
        r.run(31000);  // past the lockout
        Gate gate;
        auto stale = send("admin", "hunter22");
        CHECK(!stale->done);
        CHECK_EQ(r.set("/sys/auth/users/admin/password", "\"other-pw\"").status, Status::Ok);
        gate.release();
        CHECK(rig::drainBackground());
        r.anon.process();
        CHECK_EQ(stale->status, Status::Unauthorized);
        Status s;
        r.login("admin", "other-pw", &s);
        CHECK_EQ(s, Status::Ok);
    }
}

// The hash a stored record holds for `name` (secrets don't read back).
std::string storedHash(AuthRig& r, const char* name) {
    JsonDocument doc;
    deserializeJson(doc, r.storage.records["sys"]);
    JsonArrayConst users = doc["auth"]["users"].as<JsonArrayConst>();
    for (JsonVariantConst u : users) {
        if (std::string(u["name"] | "") == name) return u["password"] | "";
    }
    return "";
}

// Writing a password hashes it on the background task, as a sign-in does:
// not under the API lock. Until the hash lands, the new password is the
// account's, and the old hash is what's stored.
TEST(auth_password_writes_hash_off_the_lock) {
    AuthRig r;
    r.auth.defaultAdmin("admin", "pw");
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/auth/users/admin/password", "\"old-pw\"").status, Status::Ok);
    CHECK(rig::drainBackground());
    r.run(2500);
    CHECK(r.auth.verify("old-pw", storedHash(r, "admin")));
    std::shared_ptr<rig::Reply> during;
    {
        Gate gate;  // the background task is busy
        CHECK_EQ(r.set("/sys/auth/users/admin/password", "\"new-pw\"").status, Status::Ok);
        r.run(2500);  // saved meanwhile: never the plain text
        CHECK(r.storage.records["sys"].find("new-pw") == std::string::npos);
        CHECK(r.auth.verify("old-pw", storedHash(r, "admin")));
        during = std::make_shared<rig::Reply>();
        r.anon.request(Op::Set, "/sys/auth/login", "{\"name\":\"admin\",\"password\":\"new-pw\"}",
                       [during](Status s, JsonVariantConst) {
                           during->status = s;
                           during->done = true;
                       });
        r.anon.process();
        CHECK(!during->done);
    }
    CHECK(rig::drainBackground());
    r.anon.process();
    CHECK_EQ(during->status, Status::Ok);  // the pending password was the account's
    r.run(2500);
    CHECK(r.auth.verify("new-pw", storedHash(r, "admin")));
    Status s;
    r.login("admin", "old-pw", &s);
    CHECK_EQ(s, Status::Unauthorized);
    r.login("admin", "new-pw", &s);
    CHECK_EQ(s, Status::Ok);

    // A new account whose password comes before its name in the patch.
    CHECK_EQ(r.set("/sys/auth/users", "[{\"name\":\"admin\"},{\"password\":\"carl-pw\",\"name\":\"carl\"}]").status,
             Status::Ok);
    CHECK(rig::drainBackground());
    r.run(2500);
    CHECK(r.auth.verify("carl-pw", storedHash(r, "carl")));
    r.login("carl", "carl-pw", &s);
    CHECK_EQ(s, Status::Ok);

    // A refused patch changes nothing, the pending password included.
    CHECK_EQ(r.set("/sys/auth/users/carl", "{\"password\":\"x\",\"role\":\"boss\"}").status, Status::InvalidValue);
    r.login("carl", "x", &s);
    CHECK_EQ(s, Status::Unauthorized);
    r.login("carl", "carl-pw", &s);
    CHECK_EQ(s, Status::Ok);
}

// With the background queue full, a password is hashed at once, as before.
TEST(auth_password_write_with_a_full_queue) {
    AuthRig r;
    r.auth.defaultAdmin("admin", "old-pw");
    CHECK(r.dev.begin());
    {
        Gate gate;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));  // the gate runs: 8 places left
        int queued = 0;
        while (queued < 16 && tesser::background([] {})) queued++;
        CHECK_EQ(r.set("/sys/auth/users/admin/password", "\"new-pw\"").status, Status::Ok);
        r.run(2500);
        CHECK(r.auth.verify("new-pw", storedHash(r, "admin")));
    }
    CHECK(rig::drainBackground());
}

TEST(auth_whoami_and_schema) {
    AuthRig r;
    r.auth.defaultAdmin("admin", "pw");
    CHECK(r.dev.begin());
    rig::Reply w = r.request(r.anon, Op::Set, "/sys/auth/whoami");
    CHECK_EQ(w.body, "{\"access\":\"public\"}");
    w = r.request(r.admin, Op::Set, "/sys/auth/whoami");
    CHECK_EQ(w.body, "{\"access\":\"admin\"}");
    rig::Reply s = r.schema("/sys/auth");
    CHECK_EQ(at(s.body, "children/login/ui/auth"), "\"login\"");
    CHECK_EQ(at(s.body, "children/login/arg"), "\"object\"");
    CHECK_EQ(at(s.body, "children/login/params/children/password/secret"), "true");
    CHECK_EQ(at(s.body, "children/whoami/ui/auth"), "\"whoami\"");
    CHECK_EQ(at(s.body, "children/users/key"), "\"name\"");
    CHECK_EQ(at(s.body, "children/key/secret"), "true");
    CHECK_EQ(at(s.body, "children/openWarning/ui/style"), "\"warning\"");
    CHECK_EQ(at(s.body, "children/openWarning/ui/visibleWhen"), "\"open=true\"");
}

TEST(auth_backup_restores_hashes) {
    struct R : AuthRig {
        tesserkit::Backup backup{dev};
    };
    std::string saved;
    {
        R r;
        CHECK(r.dev.begin());
        r.set("/sys/auth/users", "[{\"name\":\"admin\",\"password\":\"kept-password\",\"role\":\"admin\"}]");
        CHECK(rig::drainBackground());
        saved = r.download("/sys/backup/settings").body;
    }
    R r;
    CHECK(r.dev.begin());
    CHECK(r.auth.open());
    CHECK_EQ(r.upload("/sys/backup/settings", saved).status, Status::Ok);
    CHECK(!r.auth.open());
    Status s;
    r.login("admin", "kept-password", &s);  // the hash was restored, not hashed again
    CHECK_EQ(s, Status::Ok);
}

}  // namespace auth_cases
