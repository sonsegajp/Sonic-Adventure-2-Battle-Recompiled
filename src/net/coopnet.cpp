#include "net/coopnet.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

namespace coopnet {

namespace {
constexpr char kMagic[4] = {'S', 'A', '2', 'C'};
constexpr uint8_t kHello = 1, kPing = 2, kPong = 3, kBye = 4;
constexpr const char* kCodeChars = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr const char* kRelayPrefix = "SA2B-";  // our sessions apart from the relay's other users

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint64_t nowUs() { return uint64_t(now() * 1e6); }

struct WinsockInit {
    bool ok = false;
    WinsockInit() {
        WSADATA d;
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
};
bool winsock() {
    static WinsockInit s_init;
    return s_init.ok;
}
}  // namespace

const char* stateName(State s) {
    switch (s) {
    case State::Off: return "Not connected";
    case State::Registering: return "Reaching the server";
    case State::Waiting: return "Waiting for your partner";
    case State::Connected: return "Connected";
    case State::Lost: return "Partner not responding";
    case State::Failed: return "Failed";
    }
    return "";
}

std::string newCode() {
    std::random_device rd;
    std::string c;
    for (int i = 0; i < 6; i++) c += kCodeChars[rd() % 32];
    return c;
}

std::string cleanCode(const std::string& typed) {
    std::string c;
    for (char ch : typed) {
        if (ch == ' ' || ch == '-' || ch == '\t') continue;
        if (ch >= 'a' && ch <= 'z') ch = char(ch - 'a' + 'A');
        c += ch;
    }
    if (c.rfind("SA2B", 0) == 0 && c.size() == 10) c.erase(0, 4);  // pasted with the relay prefix
    if (c.size() != 6) return {};
    for (char ch : c)
        if (!std::strchr(kCodeChars, ch)) return {};
    return c;
}

Link::~Link() { stop(); }

std::string Link::code() const {
    std::lock_guard g(m_lock);
    return m_code;
}
std::string Link::partnerName() const {
    std::lock_guard g(m_lock);
    return m_partner;
}
std::string Link::error() const {
    std::lock_guard g(m_lock);
    return m_error;
}
void Link::setError(const std::string& e) {
    std::lock_guard g(m_lock);
    m_error = e;
}

void Link::start(bool host, const std::string& code, const std::string& name, const Relay& relay) {
    stop();
    {
        std::lock_guard g(m_lock);
        m_host = host, m_code = code, m_name = name.substr(0, 24), m_relay = relay;
        m_partner.clear(), m_error.clear(), m_inbox.clear();
    }
    m_ping = -1;
    m_stop = false;
    m_state = State::Registering;
    m_thread = std::thread([this] { run(); });
}

void Link::stop() {
    if (!m_thread.joinable()) return;
    m_stop = true;
    m_thread.join();
    m_state = State::Off;
}

void Link::sendRaw(const void* data, size_t size) {
    if (m_socket == ~uintptr_t(0) || m_relayAddr.empty()) return;
    sendto(SOCKET(m_socket), static_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(m_relayAddr.data()),
           int(m_relayAddr.size()));
}

void Link::send(uint8_t type, const void* data, size_t size) {
    if (m_state != State::Connected || size > 1400) return;
    uint8_t buf[1408];
    std::memcpy(buf, kMagic, 4);
    buf[4] = type;
    std::memcpy(buf + 5, data, size);
    sendRaw(buf, size + 5);
}

bool Link::receive(uint8_t& type, std::vector<uint8_t>& data) {
    std::lock_guard g(m_lock);
    if (m_inbox.empty()) return false;
    type = m_inbox.front().first;
    data = std::move(m_inbox.front().second);
    m_inbox.pop_front();
    return true;
}

void Link::run() {
    if (!winsock()) {
        setError("networking is not available");
        m_state = State::Failed;
        return;
    }
    // the relay's address
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    const std::string port = std::to_string(m_relay.port);
    if (getaddrinfo(m_relay.host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        setError("can't find the server " + m_relay.host);
        m_state = State::Failed;
        return;
    }
    m_relayAddr.assign(reinterpret_cast<const uint8_t*>(res->ai_addr), reinterpret_cast<const uint8_t*>(res->ai_addr) + res->ai_addrlen);
    freeaddrinfo(res);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        setError("can't open a network socket");
        m_state = State::Failed;
        return;
    }
    m_socket = uintptr_t(s);

    const std::string reg = std::string("ODYNPU ") + (m_host ? "HOST " : "JOIN ") + kRelayPrefix + m_code;
    double lastReg = -100, lastHello = -100, lastPing = -100, lastHeard = 0;
    bool registered = false, gotHello = false;
    auto hello = [&] {
        uint8_t b[64] = {'S', 'A', '2', 'C', kHello};
        b[5] = uint8_t(kProtocol >> 8), b[6] = uint8_t(kProtocol), b[7] = m_host ? 0 : 1;
        const size_t n = std::min<size_t>(m_name.size(), 24);
        b[8] = uint8_t(n);
        std::memcpy(b + 9, m_name.data(), n);
        sendRaw(b, 9 + n);
    };
    const double started = now();
    while (!m_stop) {
        const double t = now();
        // keep our place at the relay (it forgets a peer after a minute of silence)
        if (t - lastReg > (registered ? 10.0 : 1.0)) {
            sendRaw(reg.data(), reg.size());
            lastReg = t;
        }
        if (!registered && t - started > 15) {
            setError("no answer from the server (" + m_relay.host + ")");
            m_state = State::Failed;
        }
        const State st = m_state;
        if (registered && st != State::Connected && t - lastHello > 0.5) hello(), lastHello = t;
        if (st == State::Connected && t - lastPing > 1.0) {
            uint8_t b[13] = {'S', 'A', '2', 'C', kPing};
            const uint64_t us = nowUs();
            std::memcpy(b + 5, &us, 8);
            sendRaw(b, sizeof b);
            lastPing = t;
        }
        if (st == State::Connected && t - lastHeard > 10) {
            m_state = State::Lost;
            gotHello = false;
        }
        // whatever arrived
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(s, &rs);
        timeval tv{0, 20000};
        if (select(0, &rs, nullptr, nullptr, &tv) <= 0) continue;
        uint8_t buf[4096];
        sockaddr_in from{};
        int fromLen = sizeof from;
        const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n <= 0) continue;
        if (n == 6 && std::memcmp(buf, "ODYRDY", 6) == 0) {
            registered = true;
            if (m_state == State::Registering || m_state == State::Failed) {
                m_state = State::Waiting;
                setError({});
            }
            continue;
        }
        if (n < 5 || std::memcmp(buf, kMagic, 4) != 0) continue;
        lastHeard = t;
        const uint8_t type = buf[4];
        if (type == kHello && n >= 9) {
            const uint16_t proto = uint16_t(buf[5] << 8 | buf[6]);
            const size_t len = std::min<size_t>(buf[8], size_t(n - 9));
            if (proto != kProtocol) {
                setError("your partner has another version of the game (protocol " + std::to_string(proto) + ", this is " +
                         std::to_string(kProtocol) + ")");
                m_state = State::Failed;
                continue;
            }
            {
                std::lock_guard g(m_lock);
                m_partner.assign(reinterpret_cast<const char*>(buf + 9), len);
            }
            if (!gotHello) hello();  // make sure the partner has ours too
            gotHello = true;
            m_state = State::Connected;
        } else if (type == kPing && n >= 13) {
            buf[4] = kPong;
            sendRaw(buf, 13);
        } else if (type == kPong && n >= 13) {
            uint64_t sent;
            std::memcpy(&sent, buf + 5, 8);
            const int ms = int((nowUs() - sent) / 1000);
            const int old = m_ping;
            m_ping = old < 0 ? ms : (old * 3 + ms) / 4;
        } else if (type == kBye) {
            m_state = State::Waiting;
            gotHello = false;
            std::lock_guard g(m_lock);
            m_partner.clear();
        } else if (type >= kFirstGameMessage) {
            std::lock_guard g(m_lock);
            if (m_inbox.size() < 4096) m_inbox.emplace_back(type, std::vector<uint8_t>(buf + 5, buf + n));
        }
    }
    const uint8_t bye[5] = {'S', 'A', '2', 'C', kBye};
    sendRaw(bye, sizeof bye);
    closesocket(s);
    m_socket = ~uintptr_t(0);
}

}  // namespace coopnet
