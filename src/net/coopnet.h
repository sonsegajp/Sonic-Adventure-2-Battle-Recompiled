// Online co-op link: two players through a relay that pairs them by a code (the Odyssey master
// server's UDP relay: a peer registers with "ODYNPU HOST <code>" or "ODYNPU JOIN <code>", resent as
// a keepalive and acknowledged with "ODYRDY"; anything else it sends goes to the other peer). Both
// sides only send out, so no ports need opening at home. Used by the game (src/runtime/coop.cpp)
// and the launcher (its CO-OP page), docs/COOP.md.
//
// Our datagrams: "SA2C", a type byte, its payload. The link itself uses HELLO (protocol version,
// role, name), PING/PONG (round trip) and BYE; the game's own messages are types 16 and up, sent
// unreliably (the game resends state every frame and repeats events until they are acknowledged).
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace coopnet {

constexpr uint16_t kProtocol = 1;  // HELLO carries it; a partner with another refuses the link
constexpr uint8_t kFirstGameMessage = 16;

struct Relay {
    std::string host = "198.71.52.82";  // the Odyssey VPS
    uint16_t port = 8901;
};

enum class State {
    Off,
    Registering,  // telling the relay who we are
    Waiting,      // registered: waiting for the partner
    Connected,    // HELLO both ways
    Lost,         // nothing from the partner for a while (still trying)
    Failed,       // no network / relay unreachable / incompatible partner (see error())
};
const char* stateName(State s);

// A fresh code for hosting: six letters and digits without look-alikes (0/O, 1/I).
std::string newCode();
// What the player typed, tidied: upper case, no spaces or dashes; empty when not a valid code.
std::string cleanCode(const std::string& typed);

class Link {
public:
    ~Link();
    // Starts (or restarts) the link on its own thread. host: this side hosts `code`.
    void start(bool host, const std::string& code, const std::string& name, const Relay& relay = {});
    void stop();  // says BYE

    State state() const { return m_state.load(); }
    bool host() const { return m_host; }
    std::string code() const;
    std::string partnerName() const;
    std::string error() const;
    int pingMs() const { return m_ping.load(); }  // -1 until measured

    // Game messages (type >= kFirstGameMessage): sent at once; received ones queue up.
    void send(uint8_t type, const void* data, size_t size);
    bool receive(uint8_t& type, std::vector<uint8_t>& data);

private:
    void run();
    void sendRaw(const void* data, size_t size);
    void setError(const std::string& e);

    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<State> m_state{State::Off};
    std::atomic<int> m_ping{-1};
    bool m_host = false;
    std::string m_code, m_name;
    Relay m_relay;
    mutable std::mutex m_lock;
    std::string m_partner, m_error;
    std::deque<std::pair<uint8_t, std::vector<uint8_t>>> m_inbox;
    uintptr_t m_socket = ~uintptr_t(0);
    std::vector<uint8_t> m_relayAddr;  // sockaddr_in
};

}  // namespace coopnet
