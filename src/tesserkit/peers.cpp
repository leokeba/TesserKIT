#include "tesserkit/peers.h"

#include <stdio.h>

#include "tesser/client.h"
#include "tesser/remote.h"
#include "tesser/udp_link.h"

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
    o.value("name", s.name).doc("Its name under /peers when declared by address").ui("placeholder", "from the id");
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
        .label("Account")
        .doc("This device's account on that node; empty: the account of every node")
        .ui("placeholder", "the default");
    o.value("password", s.password).secret();
}

void describe(tesser::Object& o, PeerAccount& a) {
    o.value("name", a.name).ui("placeholder", "none");
    o.value("password", a.password).secret();
}

Peers::Peers(Device& dev) : Service(dev, "peers") {}

Peers::~Peers() = default;

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
                        .doc("Signed in with on each node that has accounts, for more than its level for nodes")
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
    RemoteNode::onStateChange([&dev](RemoteNode&) {
        Event e;
        e.type = Event::Type::PeersChanged;
        dev.post(std::move(e));
    });
    // A token refused (expired, revoked): sign in again.
    RemoteNode::onAccess([&dev](RemoteNode& n, Status s, Access level) {
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
    RemoteNode::setDefaultRouteOrder(order);
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
    std::vector<State> states;
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
        if (states.size() >= kMaxNodes) continue;
        State st;
        st.id = idOf(device);
        st.name = n->name();
        st.online = n->online();
        std::vector<RemoteNode::Route> routes = n->routes();
        st.routes = namesOf(routes);
        RemoteNode::Route cur;
        if (n->route(cur)) {
            st.route = cur.name();
            st.rtt = cur.rttMs;
        }
        for (const RemoteNode::Route& r : routes) {
            if (!r.heardMs || !r.endpoint) continue;
            int32_t ago = static_cast<int32_t>((r.endpoint->now() - r.heardMs) / 1000);
            if (st.heard < 0 || ago < st.heard) st.heard = ago;
        }
        if (device.length) signIn(*n, s, nowMs, st);
        states.push_back(st);
    }
    // Toasts: a node lost, or reached again or another way. A node seen for
    // the first time isn't news.
    for (const State& st : states) {
        for (const State& old : states_) {
            if (old.name != st.name) continue;
            if (old.online && !st.online) {
                lostEvent_->emit(st.name);
            } else if (st.online && (!old.online || old.route != st.route)) {
                reachedEvent_->emit(st.name + " over " + st.route);
            }
        }
    }
    bool same = states.size() == states_.size();
    for (size_t i = 0; same && i < states.size(); i++) {
        const State &a = states[i], &b = states_[i];
        same = a.id == b.id && a.name == b.name && a.online == b.online && a.route == b.route &&
               a.routes == b.routes && a.rtt == b.rtt && a.heard == b.heard && a.access == b.access &&
               a.signIn == b.signIn;
    }
    if (!same) {
        states_ = std::move(states);
        statusNode_->changed();
    }
}

Peers::SignIn& Peers::signInFor(const std::string& id) {
    for (SignIn& s : signIns_) {
        if (s.id == id) return s;
    }
    signIns_.push_back(SignIn());
    signIns_.back().id = id;
    return signIns_.back();
}

void Peers::signIn(RemoteNode& node, const PeerSettings* settings, uint32_t nowMs, State& state) {
    // Its own account, or the one of every node.
    const std::string& user = settings && !settings->user.empty() ? settings->user : account_.name;
    const std::string& password = settings && !settings->user.empty() ? settings->password : account_.password;
    SignIn& s = signInFor(state.id);
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
    if (credentials.empty()) return;
    if (s.phase == SignIn::Phase::Needed && node.online() && static_cast<int32_t>(nowMs - s.retryAtMs) >= 0) {
        s.phase = SignIn::Phase::Pending;
        s.text = "signing in as " + user;
        login(node.name(), s.id, user, password);
    }
    state.signIn = s.text;
    if (s.hasToken) state.access = tesser::toString(node.access());
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
