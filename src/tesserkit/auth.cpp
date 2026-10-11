#include "tesserkit/auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tesserkit/crypto.h"
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

std::string Auth::hash(std::string_view password) {
    uint8_t salt[kSaltBytes];
    dev_.core().random(salt, sizeof(salt));
    uint8_t h[kHashBytes];
    crypto::pbkdf2Sha256(password, salt, sizeof(salt), kIterations, h, sizeof(h));
    char iterations[12];
    snprintf(iterations, sizeof(iterations), "%u", static_cast<unsigned>(kIterations));
    return std::string(kHashPrefix) + iterations + "$" + crypto::toHex(salt, sizeof(salt)) + "$" +
           crypto::toHex(h, sizeof(h));
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
    o.label("Users").ui("icon", "lock");
    o.list("users", users_,
           [this](tesser::Object& u, User& user) {
               u.value("name", user.name);
               u.value("password", [&user] { return user.password; },
                       // A stored hash (a restored backup) is kept as it is.
                       [this, &user](const std::string& v) { user.password = isHash(v) ? v : hash(v); })
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
    keyNode_ = &o.value("key", key_).readOnly().secret().persist().readAccess(Access::Admin).ui("hidden");

    if (defaultName_ && defaultPassword_) {
        User u;
        snprintf(u.name, sizeof(u.name), "%s", defaultName_);
        u.password = hash(defaultPassword_);
        u.role = Role::Admin;
        users_.push_back(u);
    }
    dev_.api().authenticate([this](std::string_view token) { return check(token); });
}

void Auth::start() {
    if (key_.size() != 64) newKey();
    wasOpen_ = open();
}

void Auth::tick(uint32_t nowMs) {
    (void)nowMs;
    bool o = open();
    if (o != wasOpen_) {  // an admin account was added or removed
        wasOpen_ = o;
        openNode_->changed();
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
    char expiry[24];
    snprintf(expiry, sizeof(expiry), "%lld", now ? static_cast<long long>(now + sessionHours_ * 3600LL) : 0LL);
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
    long long expiry = strtoll(std::string(token.substr(exp + 1, sig - exp - 1)).c_str(), nullptr, 10);
    int64_t now = nowSeconds();
    if (expiry && now && now > expiry) return Access::Public;
    const User* u = find(token.substr(0, exp));
    if (!u) return Access::Public;
    return u->role == Role::Admin ? Access::Admin : Access::User;
}

void Auth::login(const Credentials& c, tesser::Call& call) {
    uint32_t now = static_cast<uint32_t>(dev_.core().uptimeMs());
    if (lockedUntilMs_ && static_cast<int32_t>(now - lockedUntilMs_) < 0) {
        return call.fail(Status::Busy, "too many failed sign-ins: wait 30 s");
    }
    lockedUntilMs_ = 0;
    const User* u = find(c.name);
    // The hash is computed even for an unknown name, so timing doesn't tell
    // which names exist.
    bool ok = verify(c.password, u ? u->password : hash(""));
    if (!u || !ok) {
        if (++failures_ >= kMaxFailures) {
            failures_ = 0;
            lockedUntilMs_ = now + kLockoutMs;
            if (!lockedUntilMs_) lockedUntilMs_ = 1;
        }
        return call.fail(Status::Unauthorized, "wrong name or password");
    }
    failures_ = 0;
    std::string t = token(u->name);
    const char* level = u->role == Role::Admin ? "admin" : "user";
    call.replyWith([&t, level](tesser::JsonWriter& w) {
        w.beginObject();
        w.key("token");
        w.string(t);
        w.key("access");
        w.string(level);
        w.endObject();
    });
}

}  // namespace tesserkit
