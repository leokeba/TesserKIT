#include "tesserkit/auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <array>
#include <chrono>
#include <thread>

#include "tesserkit/crypto.h"
#include "tesserkit/limits.h"
#include "tesserkit/time_service.h"

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

constexpr const char* kHashPrefix = "$pbkdf2-sha256$";
constexpr size_t kSaltBytes = 16;
constexpr size_t kHashBytes = 32;
constexpr size_t kMacBytes = 16;  // of a token's signature
// What a sign-in with an unknown name is checked against: as slow as an
// account's hash, and matched by no password.
constexpr const char* kDummyHash =
    "$pbkdf2-sha256$2000$00000000000000000000000000000000$"
    "0000000000000000000000000000000000000000000000000000000000000000";

}  // namespace

void describe(tesser::Object& o, Credentials& c) {
    o.value("name", c.name);
    o.value("password", c.password).secret();
}

Auth& Auth::defaultAdmin(const char* name, const char* password) {
    defaultName_ = name;
    defaultPassword_ = password;
    return *this;
}

bool Auth::isHash(std::string_view s) {
    // $pbkdf2-sha256$<iterations>$<salt hex>$<hash hex>
    if (s.substr(0, strlen(kHashPrefix)) != kHashPrefix) return false;
    s.remove_prefix(strlen(kHashPrefix));
    size_t a = s.find('$');
    if (a == 0 || a == std::string_view::npos) return false;
    for (size_t i = 0; i < a; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    size_t b = s.find('$', a + 1);
    if (b == std::string_view::npos) return false;
    uint8_t salt[kSaltBytes], h[kHashBytes];
    return crypto::fromHex(s.substr(a + 1, b - a - 1), salt, kSaltBytes) &&
           crypto::fromHex(s.substr(b + 1), h, kHashBytes);
}

Auth::~Auth() {
    // A job holds `this` (and takes the device's API lock) until its very
    // end. Only tests destroy services; 5 s is far beyond a hash.
    for (int i = 0; i < 5000 && jobs_.load() > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

std::string Auth::hash(std::string_view password) {
    uint8_t salt[kSaltBytes];
    dev_.core().random(salt, sizeof(salt));
    return hashWith(password, salt);
}

std::string Auth::hashWith(std::string_view password, const uint8_t* salt) const {
    uint8_t h[kHashBytes];
    crypto::pbkdf2Sha256(password, salt, kSaltBytes, kIterations, h, sizeof(h));
    char iterations[12];
    snprintf(iterations, sizeof(iterations), "%u", static_cast<unsigned>(kIterations));
    return std::string(kHashPrefix) + iterations + "$" + crypto::toHex(salt, kSaltBytes) + "$" +
           crypto::toHex(h, sizeof(h));
}

void Auth::setPassword(User& u, const std::string& v) {
    // Under the API lock, in a patch's second pass: the validator has
    // accepted `v`, and the write can't be refused any more.
    u.write = ++writes_;
    if (isHash(v)) {  // a restored backup's: kept as it is
        u.password = v;
        u.pending.clear();
        return;
    }
    // The key derivation (about 280 ms on a classic ESP32) runs on TesserAPI's
    // background task, as a sign-in's does. Until its hash lands, `pending`
    // is the account's password (login() checks it), and what is persisted
    // and backed up is the previous hash: plain text is never stored.
    std::array<uint8_t, kSaltBytes> salt;
    dev_.core().random(salt.data(), salt.size());
    u.pending = v;
    uint32_t write = u.write;
    jobs_++;
    bool queued = tesser::background([this, salt, v, write] {
        std::string h = hashWith(v, salt.data());
        {
            tesser::Lock lock(dev_.api());
            // By the write, not the name: a new account's name may come
            // after its password in the patch, or change meanwhile.
            for (User& user : users_) {
                if (user.write != write) continue;
                user.password = std::move(h);
                user.pending.clear();
                usersNode_->changed();  // saved, with the hash
                break;
            }
        }
        jobs_--;
    });
    if (!queued) {  // a full queue (8 jobs): the old way, under the lock
        jobs_--;
        u.password = hashWith(v, salt.data());
        u.pending.clear();
    }
}

bool Auth::verify(std::string_view password, const std::string& stored) const {
    if (!isHash(stored)) return false;
    std::string_view s(stored);
    s.remove_prefix(strlen(kHashPrefix));
    size_t a = s.find('$');
    size_t b = s.find('$', a + 1);
    uint32_t iterations = static_cast<uint32_t>(strtoul(std::string(s.substr(0, a)).c_str(), nullptr, 10));
    if (!iterations || iterations > 1000000) return false;
    uint8_t salt[kSaltBytes], h[kHashBytes];
    crypto::fromHex(s.substr(a + 1, b - a - 1), salt, kSaltBytes);
    crypto::pbkdf2Sha256(password, salt, sizeof(salt), iterations, h, sizeof(h));
    return crypto::equal(crypto::toHex(h, sizeof(h)), s.substr(b + 1));
}

void Auth::describe(tesser::Object& o) {
    o.label("Users").ui("icon", "lock").ui("group", "Security");
    usersNode_ = &o.list("users", users_,
           [this](tesser::Object& u, User& user) {
               u.value("name", user.name);
               u.value("password", [&user] { return user.password; },
                       [this, &user](const std::string& v) { setPassword(user, v); })
                   .validate([](const std::string& v) {
                       return isHash(v) || (!v.empty() && v.size() <= 64)
                                  ? Check::ok()
                                  : Check::fail(Status::InvalidValue, "1 to 64 characters");
                   })
                   .secret();
               u.value("role", user.role, {"user", "admin"});
           })
        .key("name")
        .maxSize(8)
        .readAccess(Access::Admin)
        .persist()
        .label("Accounts");
    o.action("login", [this](const Credentials& c, tesser::Call& call) { login(c, call); })
        .label("Sign in")
        .ui("auth", "login")
        .ui("hidden");
    o.action("whoami", [](tesser::Call& call) {
         const char* level = tesser::toString(call.client().level());
         call.replyWith([level](tesser::JsonWriter& w) {
             w.beginObject();
             w.key("access");
             w.string(level);
             w.endObject();
         });
     })
        .ui("auth", "whoami")
        .ui("hidden");
    openNode_ = &o.value("open", [this] { return open(); })
                     .label("No admin account")
                     .doc("Every client is Admin until an account with the admin role exists")
                     .ui("hidden");
    o.value("openWarning", "Anyone on this network is Admin: add an account with the admin role.")
        .label("Open access")
        .ui("style", "warning")
        .ui("visibleWhen", "open=true");
    o.action("logoutAll", [this] { newKey(); })
        .label("Sign everyone out")
        .writeAccess(Access::Admin)
        .ui("confirm", "Sign every client out, this one included?")
        .ui("advanced");
    o.value("sessionHours", sessionHours_)
        .range(1, 720)
        .unit("h")
        .label("Sessions last")
        .writeAccess(Access::Admin)
        .persist()
        .ui("advanced");
    // Neither goes into backups: a backup holding the key could forge
    // tokens for this device, and the boot count is this device's.
    keyNode_ = &o.value("key", key_)
                    .validate(limits::atMost<64>)
                    .readOnly()
                    .secret()
                    .persist()
                    .backup(false)
                    .readAccess(Access::Admin)
                    .ui("hidden");
    bootsNode_ = &o.value("boots", boots_).readOnly().persist().backup(false).readAccess(Access::Admin).ui("hidden");

    if (defaultName_ && defaultPassword_) {
        User u;
        snprintf(u.name, sizeof(u.name), "%s", defaultName_);
        u.password = hash(defaultPassword_);
        u.role = Role::Admin;
        users_.push_back(u);
    }
    dev_.api().authenticate([this](std::string_view token) { return check(token); });
}

void Auth::loaded() {
    // Before any service starts: Network raises its access point in its
    // start(), with a password in secure mode, and services come in
    // registration order, Network usually before Auth.
    wasOpen_ = open();
    dev_.secure(!wasOpen_);
}

void Auth::start() {
    if (key_.size() != 64) newKey();
    boots_++;  // a token made without a clock is valid in its boot only
    bootsNode_->changed();
}

void Auth::tick(uint32_t nowMs) {
    (void)nowMs;
    bool o = open();
    if (o != wasOpen_) {  // an admin account was added or removed
        wasOpen_ = o;
        dev_.secure(!o);
        openNode_->changed();
    }
    // An account added, removed or changed (a password, a role), every
    // token revoked, or the device closed: the clients that hold a level
    // from a token (or from the open device) are checked again, so an open
    // WebSocket doesn't keep a level it lost (TesserAPI's tokensChanged()).
    uint32_t gen = usersNode_->generation() + keyNode_->generation() + (o ? 0u : 1u);
    if (gen != tokensGen_) {
        tokensGen_ = gen;
        dev_.api().tokensChanged();
    }
}

void Auth::newKey() {
    uint8_t k[32];
    dev_.core().random(k, sizeof(k));
    key_ = crypto::toHex(k, sizeof(k));
    keyNode_->changed();  // persisted: tokens survive restarts, until the next key
}

bool Auth::open() const {
    for (const User& u : users_) {
        if (u.role == Role::Admin) return false;
    }
    return true;
}

const User* Auth::find(std::string_view name) const {
    for (const User& u : users_) {
        if (name == u.name) return &u;
    }
    return nullptr;
}

int64_t Auth::nowSeconds() const {
    Clock* clock = dev_.platform().clock;
    int64_t ms = clock ? clock->epochMs() : 0;
    return ms >= Time::kValidAfterMs ? ms / 1000 : 0;  // 0: the clock isn't set
}

std::string Auth::sign(std::string_view payload) const {
    uint8_t mac[32];
    crypto::hmacSha256(key_.data(), key_.size(), payload.data(), payload.size(), mac);
    return crypto::toHex(mac, kMacBytes);
}

std::string Auth::token(const std::string& name) {
    int64_t now = nowSeconds();
    char expiry[32];
    if (now) {
        snprintf(expiry, sizeof(expiry), "%lld", static_cast<long long>(now + sessionHours_ * 3600LL));
    } else {
        // No clock (a show network without SNTP): "b<boot>:<uptime deadline>",
        // so the token dies with this boot or after sessionHours of uptime.
        // It used to say 0, never expiring.
        long long deadline = static_cast<long long>(dev_.core().uptimeMs() / 1000) + sessionHours_ * 3600LL;
        snprintf(expiry, sizeof(expiry), "b%lu:%lld", static_cast<unsigned long>(boots_), deadline);
    }
    std::string payload = name + "." + expiry;
    return payload + "." + sign(payload);
}

Access Auth::check(std::string_view token) {
    tesser::Lock lock(dev_.api());
    if (open()) return Access::Admin;
    // <name>.<expiry>.<signature>; names may hold dots.
    size_t sig = token.rfind('.');
    if (sig == std::string_view::npos || sig == 0) return Access::Public;
    size_t exp = token.rfind('.', sig - 1);
    if (exp == std::string_view::npos || exp == 0) return Access::Public;
    std::string_view payload = token.substr(0, sig);
    if (!crypto::equal(sign(payload), token.substr(sig + 1))) return Access::Public;
    std::string expiry(token.substr(exp + 1, sig - exp - 1));
    if (expiry[0] == 'b') {
        // Made without a clock: this boot, before its uptime deadline.
        unsigned long boot = 0;
        long long deadline = 0;
        if (sscanf(expiry.c_str(), "b%lu:%lld", &boot, &deadline) != 2 || boot != boots_ ||
            static_cast<long long>(dev_.core().uptimeMs() / 1000) > deadline) {
            return Access::Public;
        }
    } else {
        long long at = strtoll(expiry.c_str(), nullptr, 10);
        if (!at) return Access::Public;  // an old clockless token, which never expired
        int64_t now = nowSeconds();
        if (now && now > at) return Access::Public;
    }
    const User* u = find(token.substr(0, exp));
    if (!u) return Access::Public;
    return u->role == Role::Admin ? Access::Admin : Access::User;
}

void Auth::login(const Credentials& c, tesser::Call& call) {
    // Under the API lock: the lockout, and a copy of what the check needs.
    // The key derivation (about 280 ms on a classic ESP32) runs on
    // TesserAPI's background task without the lock, so every transport goes
    // on meanwhile (docs/DESIGN.md section 5.7).
    uint32_t now = static_cast<uint32_t>(dev_.core().uptimeMs());
    Attempts* a = attemptsOf(call.client(), now);
    if (!a) return call.fail(Status::Busy, "too many clients signing in: try again");
    if (a->lockedUntilMs && static_cast<int32_t>(now - a->lockedUntilMs) < 0) {
        return call.fail(Status::Busy, "too many failed sign-ins: wait 30 s");
    }
    a->lockedUntilMs = 0;
    // Sign-ins being checked count as failures to come, so no more than
    // kMaxFailures are tried before the lockout.
    if (a->failures + a->checking >= kMaxFailures) return call.fail(Status::Busy, "sign-ins being checked: try again");
    Attempts who = *a;  // its client, found again when the check is done
    const User* u = find(c.name);
    std::string stored = u ? u->password : std::string();
    std::string pending = u ? u->pending : std::string();  // a new password not hashed yet
    uint32_t write = u ? u->write : 0;
    tesser::Pending p = call.defer();
    if (!p) return;  // refused (too many pending, or a transport that can't wait): the call says why
    a->checking++;
    jobs_++;
    bool queued = tesser::background([this, p, name = c.name, password = c.password, stored, pending, write,
                                      now, who]() mutable {
        // A hash is computed even for an unknown name, or a password not
        // hashed yet, so timing doesn't tell which names exist.
        bool ok;
        if (!pending.empty()) {
            verify(password, kDummyHash);
            ok = crypto::equal(password, pending);
        } else {
            ok = verify(password, stored.empty() ? std::string(kDummyHash) : stored) && !stored.empty();
        }
        std::string t;
        const char* level = "user";
        {
            tesser::Lock lock(dev_.api());
            Attempts* client = nullptr;
            for (Attempts& e : attempts_) {
                if (e.length == who.length && memcmp(e.client, who.client, who.length) == 0) client = &e;
            }
            if (client) client->checking--;
            const User* user = find(name);
            ok = ok && user && user->write == write;  // the password wasn't written meanwhile
            if (ok) {
                if (client) client->failures = 0;
                t = token(user->name);
                if (user->role == Role::Admin) level = "admin";
            } else if (client && ++client->failures >= kMaxFailures) {
                client->failures = 0;
                client->lockedUntilMs = now + kLockoutMs;
                if (!client->lockedUntilMs) client->lockedUntilMs = 1;
            }
        }
        // Answered without the lock: a Pending may be answered from any task.
        if (!ok) {
            p.fail(Status::Unauthorized, "wrong name or password");
        } else {
            p.replyWith([&t, level](tesser::JsonWriter& w) {
                w.beginObject();
                w.key("token");
                w.string(t);
                w.key("access");
                w.string(level);
                w.endObject();
            });
        }
        jobs_--;
    });
    if (!queued) {
        a->checking--;
        jobs_--;
        p.fail(Status::Busy, "busy");
    }
}

Auth::Attempts* Auth::attemptsOf(const tesser::Client& c, uint32_t nowMs) {
    // The client: its transport and its address (an IP, a MAC; none for a
    // local or serial client, which share one entry).
    uint8_t key[sizeof(Attempts::client)];
    key[0] = static_cast<uint8_t>(c.transport);
    uint8_t n = c.addressLength < sizeof(key) - 1 ? c.addressLength : static_cast<uint8_t>(sizeof(key) - 1);
    memcpy(key + 1, c.address, n);
    uint8_t length = static_cast<uint8_t>(n + 1);
    for (Attempts& e : attempts_) {
        if (e.length == length && memcmp(e.client, key, length) == 0) {
            e.lastMs = nowMs;
            return &e;
        }
    }
    // A new client: a free entry, or the oldest one that is neither locked
    // nor being checked. None: every entry is in use, refused for now.
    Attempts* slot = nullptr;
    if (attempts_.size() < kClients) {
        attempts_.emplace_back();
        slot = &attempts_.back();
    } else {
        for (Attempts& e : attempts_) {
            bool locked = e.lockedUntilMs && static_cast<int32_t>(nowMs - e.lockedUntilMs) < 0;
            if (locked || e.checking) continue;
            if (!slot || static_cast<int32_t>(e.lastMs - slot->lastMs) < 0) slot = &e;
        }
        if (!slot) return nullptr;
        *slot = Attempts();
    }
    memcpy(slot->client, key, length);
    slot->length = length;
    slot->lastMs = nowMs;
    return slot;
}

}  // namespace tesserkit
