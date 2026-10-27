#include "tesserkit/peers.h"

#include <stdio.h>

#include <algorithm>

#include "tesser/budget.h"
#include "tesser/client.h"
#include "tesser/remote.h"
#include "tesser/udp_link.h"

#include "tesserkit/limits.h"

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::PeerAddress;
using tesser::RemoteNode;
using tesser::RouteKind;
using tesser::Status;
using tesser::TransportKind;

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "10521c75f37c" or "10:52:1c:75:f3:7c", any case.
bool parseId(const std::string& text, PeerAddress& out) {
    uint8_t b[6];
    size_t digits = 0;
    for (char c : text) {
        if (c == ':') continue;
        int d = hexDigit(c);
        if (d < 0 || digits >= 12) return false;
        if (digits % 2 == 0) {
            b[digits / 2] = static_cast<uint8_t>(d << 4);
        } else {
            b[digits / 2] = static_cast<uint8_t>(b[digits / 2] | d);
        }
        digits++;
    }
    if (digits != 12) return false;
    out = PeerAddress::fromBytes(b, 6);
    return true;
}

std::string idOf(const PeerAddress& device) {
    std::string s;
    char hex[3];
    for (uint8_t i = 0; i < device.length; i++) {
        snprintf(hex, sizeof(hex), "%02x", device.bytes[i]);
        s += hex;
    }
    return s;
}

// "udp/ethernet, nowtp": TesserAPI's route names (udp on any interface,
// udp/ethernet, udp/wifi, udp/ap, nowtp), each once. Empty is valid.
bool parseOrder(const std::string& text, std::vector<RouteKind>& out) {
    std::vector<RouteKind> order;
    if (!tesser::parseRouteOrder(text, order)) return false;
    for (size_t i = 0; i < order.size(); i++) {
        TransportKind t = order[i].transport;
        if (t != TransportKind::Udp && t != TransportKind::NowTP) return false;
        if (t == TransportKind::NowTP && order[i].iface != tesser::NetInterface::Any) return false;
        for (size_t j = 0; j < i; j++) {
            if (order[j] == order[i]) return false;
        }
    }
    out = std::move(order);
    return true;
}

Check checkOrder(const std::string& v) {
    std::vector<RouteKind> order;
    return parseOrder(v, order) ? Check::ok()
                                : Check::fail(Status::InvalidValue,
                                              "routes in order: udp/ethernet, udp/wifi, udp/ap, udp (any), nowtp");
}

// The round trip moved far enough to notify: by half of what was shown, and
// at least 20 ms, or to or from none.
bool rttMoved(uint32_t shown, uint32_t now) {
    if ((shown == 0) != (now == 0)) return true;
    uint32_t moved = shown > now ? shown - now : now - shown;
    return moved >= std::max<uint32_t>(shown / 2, 20);
}

// Seconds since heard, in the steps a subscriber is told of: never, under
// 5 s (heard as usual), 30 s, 2 min, 10 min, longer.
int heardBucket(int32_t s) { return s < 0 ? 0 : s < 5 ? 1 : s < 30 ? 2 : s < 120 ? 3 : s < 600 ? 4 : 5; }

std::string namesOf(const std::vector<RemoteNode::Route>& routes) {
    std::string s;
    for (const RemoteNode::Route& r : routes) {
        if (!s.empty()) s += ", ";
        s += r.name();
        if (!r.online) s += " (offline)";
    }
    return s;
}

}  // namespace

void describe(tesser::Object& o, PeerSettings& s) {
    o.value("id", s.id)
        .label("Device")
        .doc("Its station MAC, as /peers shows it (id)")
        .validate([](const std::string& v) {
            PeerAddress a;
            return parseId(v, a) ? Check::ok() : Check::fail(Status::InvalidValue, "a MAC: 12 hex digits");
        })
        .ui("placeholder", "10521c75f37c");
    o.value("name", s.name).validate(limits::atMost<limits::kName>).doc("Its name under /peers when declared by address").ui("placeholder", "from the id");
    o.value("order", s.order).label("Routes").validate(checkOrder).ui("placeholder", "the default");
    o.value("address", s.address)
        .doc("Reached over UDP at this address even when it isn't discovered (another subnet)")
        .validate([](const std::string& v) {
            PeerAddress a;
            return v.empty() || tesser::parseUdpAddress(v, 8484, a)
                       ? Check::ok()
                       : Check::fail(Status::InvalidValue, "an IPv4 address, with a port or not");
        })
        .ui("placeholder", "discovered");
    o.value("user", s.user)
        .validate(limits::atMost<limits::kName>)
        .label("Account")
        .doc("This device's account on that node; empty: the account of every node")
        .ui("placeholder", "the default");
    // Not in backups: a backup would clone the fleet's password.
    o.value("password", s.password).validate(limits::atMost<limits::kSecret>).secret().backup(false);
}

void describe(tesser::Object& o, PeerAccount& a) {
    o.value("name", a.name).validate(limits::atMost<limits::kName>).ui("placeholder", "none");
    o.value("password", a.password).validate(limits::atMost<limits::kSecret>).secret().backup(false);
}

Peers::Peers(Device& dev) : Service(dev, "peers") {}

Peers::~Peers() {
    tesser::RemoteHooks& hooks = dev_.api().remoteHooks();
    if (stateHook_) hooks.remove(stateHook_);
    if (accessHook_) hooks.remove(accessHook_);
}

Peers& Peers::order(const char* order) {
    order_ = order ? order : "";
    return *this;
}

Peers& Peers::account(const char* name, const char* password) {
    account_.name = name ? name : "";
    account_.password = password ? password : "";
    return *this;
}

Peers& Peers::onDeviceLost(DeviceLostHandler fn) {
    lost_ = std::move(fn);
    return *this;
}

tesser::Object& Peers::nodes() {
    if (!peers_) peers_ = &dev_.api().object("peers").label("Nodes nearby").ui("icon", "target");
    return *peers_;
}

void Peers::describe(tesser::Object& o) {
    o.label("Nodes")
        .ui("group", "Nodes")
        .ui("icon", "target")
        .doc("How this device reaches the nodes under /peers: over UDP, NowTP, or both in order")
        .writeAccess(Access::Admin);
    orderNode_ = &o.value("order", order_)
                      .validate(checkOrder)
                      .persist()
                      .label("Routes")
                      .doc("The routes tried for each node, in order, the next one when a node is lost on one: "
                           "udp/ethernet, udp/wifi, udp/ap, udp (any interface), nowtp")
                      .ui("placeholder", "in the order the nodes were found");
    accountNode_ = &o.mount("account", account_)
                        .persist()
                        .label("Account on the nodes")
                        .doc("The account this device signs in with on the nodes that have accounts. Without one, it has their "
                             "level for nodes")
                        .ui("form");
    settingsNode_ =
        &o.list("nodes", settings_).key("id").maxSize(kMaxNodes).persist().label("Settings per node").ui("advanced");
    statusNode_ = &o.list("status", states_,
                          [](tesser::Object& e, State& s) {
                              e.value("id", static_cast<const std::string&>(s.id)).label("Device");
                              e.value("name", static_cast<const std::string&>(s.name));
                              e.value("online", static_cast<const bool&>(s.online));
                              e.value("route", static_cast<const std::string&>(s.route)).label("Over");
                              e.value("routes", static_cast<const std::string&>(s.routes))
                                  .label("Routes")
                                  .doc("Every route to it, in the order tried; \"(offline)\" after one that lost it");
                              e.value("rtt", static_cast<const uint32_t&>(s.rtt)).unit("ms").label("Round trip");
                              e.value("heard", static_cast<const int32_t&>(s.heard))
                                  .unit("s")
                                  .label("Last heard")
                                  .doc("Seconds ago; -1: never");
                              e.value("access", static_cast<const std::string&>(s.access))
                                  .doc("The level the node grants this device's token");
                              e.value("signIn", static_cast<const std::string&>(s.signIn)).label("Sign-in");
                          })
                       .key("id")
                       .maxSize(kMaxNodes)
                       .readOnly()
                       .label("Nodes");
    lostEvent_ = &o.event("lost").label("Lost").ui("toast", "warn").ui("hidden");
    reachedEvent_ = &o.event("reached").label("Reached").ui("toast", "info").ui("hidden");
}

void Peers::start() {
    nodes();
    orderGen_ = orderNode_->generation();
    settingsGen_ = settingsNode_->generation();
    applyDefaultOrder();
    Device& dev = dev_;
    // One hook for every node: wakes refresh() rather than touching the tree
    // from the endpoint's task.
    // This device's Api's hooks, not the process-wide ones: a second Device
    // or Peers (a simulator, two-device tests) keeps its own.
    tesser::RemoteHooks& hooks = dev_.api().remoteHooks();
    stateHook_ = hooks.onStateChange([&dev](RemoteNode&) {
        Event e;
        e.type = Event::Type::PeersChanged;
        dev.post(std::move(e));
    });
    // A token refused (expired, revoked): sign in again.
    accessHook_ = hooks.onAccess([&dev](RemoteNode& n, Status s, Access level) {
        if (s != Status::Ok || level != Access::Public) return;
        PeerAddress device = n.device();
        Event e;
        e.type = Event::Type::PeerRefused;
        e.text.assign(reinterpret_cast<const char*>(device.bytes), device.length);
        dev.post(std::move(e));
    });
    client_.reset(new tesser::LocalClient(dev_.api()));  // Admin: this device's own requests
    accountGen_ = accountNode_->generation();
    dirty_ = true;
}

void Peers::watch(Device& dev, tesser::DatagramEndpoint& endpoint) {
    // Set once, as the endpoint is created: no peer can be lost before.
    endpoint.onPeerLost([&dev](const PeerAddress&, const PeerAddress& device, tesser::PeerLoss reason) {
        if (!device.length) return;
        Event e;
        e.type = Event::Type::DeviceLost;
        e.value = static_cast<int32_t>(reason);
        e.text.assign(reinterpret_cast<const char*>(device.bytes), device.length);
        dev.post(std::move(e));
    });
}

std::vector<tesser::DatagramEndpoint*> Peers::endpoints() const {
    std::vector<tesser::DatagramEndpoint*> out;
    Platform& p = dev_.platform();
    if (p.udpApi && p.udpApi->link()) out.push_back(&p.udpApi->link()->endpoint());
    if (p.nowtp && p.nowtp->endpoint()) out.push_back(p.nowtp->endpoint());
    return out;
}

void Peers::applyDefaultOrder() {
    std::vector<RouteKind> order;
    if (!parseOrder(order_, order)) order.clear();  // a bad factory default: the order found
    dev_.api().remoteHooks().setRouteOrder(order);
}

const PeerSettings* Peers::settingsFor(const PeerAddress& device) const {
    if (!device.length) return nullptr;
    for (const PeerSettings& s : settings_) {
        PeerAddress a;
        if (parseId(s.id, a) && a == device) return &s;
    }
    return nullptr;
}

void Peers::declare() {
    Platform& p = dev_.platform();
    tesser::UdpLink* link = p.udpApi ? p.udpApi->link() : nullptr;
    if (!link) return;
    tesser::DatagramEndpoint& ep = link->endpoint();
    for (const PeerSettings& s : settings_) {
        if (s.address.empty()) continue;
        std::string done = s.id + " " + s.address;
        bool seen = false;
        for (const std::string& d : declared_) seen = seen || d == done;
        if (seen) continue;
        PeerAddress device, address;
        if (!parseId(s.id, device) || !tesser::parseUdpAddress(s.address, 8484, address)) continue;
        declared_.push_back(done);
        if (RemoteNode* n = RemoteNode::byDevice(*peers_, device)) {
            // Found already: one more way to reach it, unless UDP has one.
            bool udp = false;
            for (const RemoteNode::Route& r : n->routes()) udp = udp || r.endpoint == &ep;
            if (!udp) n->addRoute(ep, address);
            continue;
        }
        std::string name = s.name.empty() ? "node-" + idOf(device).substr(6) : s.name;
        if (peers_->child(name)) name += "-" + idOf(device).substr(6);
        if (peers_->child(name)) {
            tesser::logWarning("tesserkit: /peers/%s is taken", name.c_str());
            continue;
        }
        RemoteNode& n = peers_->remote(name, ep, address);  // keeps a copy of the name
        n.setDevice(device);
        n.changed();
    }
}

void Peers::refresh(uint32_t nowMs) {
    refreshedMs_ = nowMs;
    dirty_ = false;
    declare();
    // The status is updated from the heap (a node new to it, a route's
    // text): reserved first, so a short heap skips it (the previous one
    // stands, tried again at the next period) rather than aborting in the
    // device's task (DEVKIT-V3 as a gateway, 2026-10-15). Routes and sign-ins
    // below go on regardless.
    size_t count = 0;
    for (tesser::Node* c = peers_->first(); c && count < kMaxNodes; c = c->next()) {
        if (c->type() == tesser::NodeType::Remote) count++;
    }
    tesser::Reservation held = tesser::reserveBytes(count * kStateBytes);
    bool status = static_cast<bool>(held);
    // In place, by name: the round trip and the time since a node was heard
    // move most seconds, and notify the list (whole, to every subscriber)
    // only when they move far (rttMoved(), heardBucket()).
    if (status) {
        for (State& st : states_) st.seen = false;
    }
    bool listChanged = false;
    size_t shown = 0;
    for (tesser::Node* c = peers_->first(); c; c = c->next()) {
        if (c->type() != tesser::NodeType::Remote) continue;
        auto* n = static_cast<RemoteNode*>(c);
        PeerAddress device = n->device();
        // Each node's own order, or the default (applied as soon as the device
        // is known, whenever it appears).
        std::vector<RouteKind> order;
        const PeerSettings* s = settingsFor(device);
        if (s && !parseOrder(s->order, order)) order.clear();
        if (n->routeOrder() != order) n->routeOrder(order);
        if (shown >= kMaxNodes) continue;
        shown++;
        std::string id = idOf(device);  // 12 characters: no allocation
        if (!status) {
            // No room for the status: none of it is updated (its strings, the
            // routes' copy), but sign-ins go on.
            if (device.length) signIn(*n, s, nowMs, id, nullptr);
            continue;
        }
        State* st = nullptr;
        for (State& old : states_) {
            if (old.name == n->name()) st = &old;
        }
        bool fresh = !st;
        if (fresh) {
            states_.emplace_back();
            st = &states_.back();
            st->name = n->name();
            listChanged = true;
        }
        st->seen = true;
        bool online = n->online();
        std::vector<RemoteNode::Route> routes = n->routes();
        std::string names = namesOf(routes);
        std::string route;
        uint32_t rtt = 0;
        RemoteNode::Route cur;
        if (n->route(cur)) {
            route = cur.name();
            rtt = cur.rttMs;
        }
        int32_t heard = -1;
        for (const RemoteNode::Route& r : routes) {
            if (!r.heardMs || !r.endpoint) continue;
            int32_t ago = static_cast<int32_t>((r.endpoint->now() - r.heardMs) / 1000);
            if (heard < 0 || ago < heard) heard = ago;
        }
        // Toasts: a node lost, or reached again or another way. A node seen
        // for the first time isn't news.
        if (!fresh) {
            if (st->online && !online) {
                lostEvent_->emit(st->name);
            } else if (online && (!st->online || st->route != route)) {
                reachedEvent_->emit(st->name + " over " + route);
            }
        }
        if (st->id != id || st->online != online || st->route != route || st->routes != names) {
            st->id = id;
            st->online = online;
            st->route = std::move(route);
            st->routes = std::move(names);
            listChanged = true;
        }
        st->rtt = rtt;
        st->heard = heard;
        if (rttMoved(st->rttShown, rtt) || heardBucket(st->heardShown) != heardBucket(heard)) listChanged = true;
        if (device.length && signIn(*n, s, nowMs, id, st)) listChanged = true;
    }
    if (!status) return;
    size_t before = states_.size();
    states_.erase(std::remove_if(states_.begin(), states_.end(), [](const State& st) { return !st.seen; }),
                  states_.end());
    if (states_.size() != before) listChanged = true;
    if (listChanged) {
        for (State& st : states_) {
            st.rttShown = st.rtt;
            st.heardShown = st.heard;
        }
        statusNode_->changed();
    }
    held.settle();
}

Peers::SignIn& Peers::signInFor(const std::string& id) {
    for (SignIn& s : signIns_) {
        if (s.id == id) return s;
    }
    signIns_.push_back(SignIn());
    signIns_.back().id = id;
    return signIns_.back();
}

bool Peers::signIn(RemoteNode& node, const PeerSettings* settings, uint32_t nowMs, const std::string& id,
                   State* state) {
    // Its own account, or the one of every node.
    const std::string& user = settings && !settings->user.empty() ? settings->user : account_.name;
    const std::string& password = settings && !settings->user.empty() ? settings->password : account_.password;
    SignIn& s = signInFor(id);
    std::string credentials = user.empty() ? std::string() : user + '\n' + password;
    if (credentials != s.credentials) {
        // New credentials (or none any more): the old token goes.
        if (s.hasToken) node.setToken("");
        s.credentials = credentials;
        s.phase = SignIn::Phase::Needed;
        s.retryAtMs = nowMs;
        s.hasToken = false;
        s.text.clear();
    }
    if (credentials.empty()) return state && setSignIn(*state, "", "");
    if (s.phase == SignIn::Phase::Needed && node.online() && static_cast<int32_t>(nowMs - s.retryAtMs) >= 0) {
        s.phase = SignIn::Phase::Pending;
        s.text = "signing in as " + user;
        login(node.name(), s.id, user, password);
    }
    if (!state) return false;
    return setSignIn(*state, s.text.c_str(), s.hasToken ? tesser::toString(node.access()) : "");
}

bool Peers::setSignIn(State& state, const char* text, const char* access) {
    if (state.signIn == text && state.access == access) return false;
    state.signIn = text;
    state.access = access;
    return true;
}

void Peers::login(const std::string& nodeName, const std::string& id, const std::string& user,
                  const std::string& password) {
    JsonDocument doc;
    doc["name"] = user;
    doc["password"] = password;
    std::string body;
    serializeJson(doc, body);
    std::string path = "/peers/" + nodeName + "/sys/auth/login";
    client_->request(tesser::Op::Set, path, body, [this, id, user](Status st, JsonVariantConst reply) {
        // In client_->process(), from tick(): under the API lock.
        uint32_t now = static_cast<uint32_t>(dev_.core().uptimeMs());
        SignIn& s = signInFor(id);
        if (s.phase != SignIn::Phase::Pending) return;  // the credentials changed meanwhile
        PeerAddress device;
        RemoteNode* node = parseId(id, device) ? RemoteNode::byDevice(*peers_, device) : nullptr;
        const char* token = reply["token"] | "";
        if (st == Status::Ok && node && *token) {
            node->setToken(token);
            s.phase = SignIn::Phase::Done;
            s.hasToken = true;
            s.signedInMs = now;
            s.text = "signed in as " + user;
        } else if (st == Status::Unauthorized || st == Status::InvalidValue) {
            // Not again until the credentials change: the node locks sign-ins
            // out after a few failures.
            s.phase = SignIn::Phase::Stopped;
            s.text = "wrong name or password";
        } else if (st == Status::NotFound) {
            s.phase = SignIn::Phase::Stopped;
            s.text = "the node has no accounts";
        } else {
            s.phase = SignIn::Phase::Needed;
            s.retryAtMs = now + kSignInRetryMs;
            const char* why = reply["message"] | "";
            s.text = std::string("couldn't sign in: ") + (*why ? why : tesser::toString(st));
        }
        dirty_ = true;
    });
}

void Peers::tick(uint32_t nowMs) {
    if (client_) client_->process();
    uint32_t gen = orderNode_->generation();
    if (gen != orderGen_) {
        orderGen_ = gen;
        applyDefaultOrder();
        dirty_ = true;
    }
    if ((gen = settingsNode_->generation()) != settingsGen_) {
        settingsGen_ = gen;
        dirty_ = true;
    }
    if ((gen = accountNode_->generation()) != accountGen_) {
        accountGen_ = gen;
        dirty_ = true;
    }
    if (dirty_ || nowMs - refreshedMs_ >= kRefreshMs) refresh(nowMs);
}

void Peers::event(const Event& e) {
    if (e.type == Event::Type::PeersChanged) {
        dirty_ = true;
        return;
    }
    if (e.type == Event::Type::PeerRefused) {
        PeerAddress device = PeerAddress::fromBytes(reinterpret_cast<const uint8_t*>(e.text.data()),
                                                    static_cast<uint8_t>(e.text.size()));
        SignIn& s = signInFor(idOf(device));
        if (s.phase != SignIn::Phase::Done) return;
        // Expired or revoked: sign in again, at most every kSignInRetryMs if
        // new tokens keep being refused.
        uint32_t now = static_cast<uint32_t>(dev_.core().uptimeMs());
        s.phase = SignIn::Phase::Needed;
        s.retryAtMs = now - s.signedInMs < kSignInRetryMs ? s.signedInMs + kSignInRetryMs : now;
        s.text = "token refused";
        dirty_ = true;
        return;
    }
    if (e.type != Event::Type::DeviceLost) return;
    PeerAddress device =
        PeerAddress::fromBytes(reinterpret_cast<const uint8_t*>(e.text.data()), static_cast<uint8_t>(e.text.size()));
    auto reason = static_cast<tesser::PeerLoss>(e.value);
    // Its frames don't authenticate: another pairing key, or none. Told
    // even when another route reaches it, since it is a setting to fix;
    // TesserAPI tells it once until the device is heard again.
    if (reason == tesser::PeerLoss::AuthFailed) {
        std::string id = idOf(device), name = id;
        for (const State& st : states_) {
            if (st.id == id) name = st.name;
        }
        tesser::logWarning("tesserkit: %s: wrong pairing key", name.c_str());
        lostEvent_->emit(name + ": wrong pairing key");
    }
    // Gone from one transport, perhaps not from the others (a failover). A
    // restart counts even when the device is back at once: what it held is
    // gone.
    if (reason != tesser::PeerLoss::Restarted) {
        for (tesser::DatagramEndpoint* ep : endpoints()) {
            if (ep->deviceOnline(device)) return;
        }
    }
    if (lost_) lost_(device, reason);
}

}  // namespace tesserkit
