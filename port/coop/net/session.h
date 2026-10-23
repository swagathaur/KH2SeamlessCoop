// A co-op session: one host and up to two clients over UDP, star topology
// (clients talk only to the host, so only the host needs a reachable port).
// The host relays each player's state and events to the others. Call
// update() every frame; it never blocks.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "protocol.h"
#include "nat.h"
#include "udp_socket.h"

namespace kh2net
{
struct SessionConfig
{
    std::string name = "Player";
    uint32_t gameCrc = 0;      // players must run the same game build
    uint16_t port = kDefaultPort; // host: port to listen on; client: 0 = any
    double timeout = 15.0;     // seconds of silence before a peer is dropped (loading can stall a game a while)
    float simulatedLoss = 0;   // testing: drop this fraction of outgoing packets
    bool openPort = false;     // host: ask the router to forward the port (UPnP / NAT-PMP)
    bool discover = false;     // ask a public STUN server how the internet sees us (the game turns both on)
};

struct PeerInfo
{
    uint8_t id = 0;
    std::string name;
    double rttMs = 0;
    float loss = 0;
    bool local = false;
};

class Session
{
public:
    enum class Status
    {
        Offline,
        Hosting,
        Connecting,
        Connected,
        Failed, // see lastError()
    };

    Session();
    ~Session();

    bool host(const SessionConfig &config);
    bool join(const SessionConfig &config, const Address &hostAddress);
    void leave();

    // Receive, deliver callbacks, resend, keep alive, time out. now: seconds.
    void update(double now);

    // This player's state; goes to everyone (via the host) with the next update.
    void sendState(const PlayerState &state);
    // A reliable, ordered event for everyone else.
    void sendEvent(uint16_t type, const std::vector<uint8_t> &data);
    // Host: the enemies in its room, to every client.
    void sendEnemies(const EnemyStates &states);

    Status status() const { return m_status; }
    bool active() const { return m_status == Status::Hosting || m_status == Status::Connected; }
    bool isHost() const { return m_status == Status::Hosting; }
    uint8_t localId() const { return m_localId; }
    std::vector<PeerInfo> roster() const;
    const std::string &lastError() const { return m_error; }
    uint16_t port() const { return m_socket.localPort(); }
    // How the internet sees this session's socket (STUN), once known.
    const Address &publicAddress() const { return m_public; }
    // The address to give other players: the router's forwarded port when it
    // opened one, else the STUN answer (invalid until one is known).
    Address shareAddress() const;
    // Host: what the router said about forwarding the port.
    PortMapper::Status portMapping() const { return m_mapper.status(); }
    // Host: let a player in whose router does not let us in on its own (hole
    // punching): send to their public address for 30 s while they join.
    void invite(const Address &player);

    std::function<void(const PlayerState &)> onState;
    std::function<void(const EnemyStates &)> onEnemies; // clients
    std::function<void(uint8_t origin, uint16_t type, const std::vector<uint8_t> &data)> onEvent;
    std::function<void(uint8_t id, const std::string &name, bool joined)> onPeer;
    std::function<void(const std::string &)> onLog;

private:
    struct Peer;
    void receiveAll(double now);
    void handleConnect(const Address &from, ByteReader &r, double now);
    void handleData(Peer &peer, ByteReader &r, double now);
    void deliver(Peer &from, uint8_t type, const std::vector<uint8_t> &body);
    void sendRaw(const Address &to, const std::vector<uint8_t> &packet);
    void broadcastRoster();
    void dropPeer(size_t index, const char *why);
    void log(const std::string &text);

    SessionConfig m_config;
    UdpSocket m_socket;
    Status m_status = Status::Offline;
    std::string m_error;
    uint8_t m_localId = 0;
    std::vector<std::unique_ptr<Peer>> m_peers; // host: clients; client: the host
    Address m_hostAddress;
    uint32_t m_nonce = 0;
    double m_connectStart = 0, m_lastConnectSend = -1e9;
    double m_now = 0;
    std::vector<RosterEntry> m_roster;
    // NAT traversal (nat.h).
    struct StunServers;
    void stunStep(double now);
    void startDiscovery();
    PortMapper m_mapper;
    std::shared_ptr<StunServers> m_stunServers;
    Address m_public;
    uint8_t m_stunTx[12] = {};
    int m_stunTries = 0;
    double m_stunNext = 0;
    struct Invite
    {
        Address address;
        double until = 0, next = 0;
    };
    std::vector<Invite> m_invites;
    uint32_t m_rng = 0x12345678u;
};
} // namespace kh2net
