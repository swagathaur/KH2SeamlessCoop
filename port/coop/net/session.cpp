#include "session.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "channel.h"

namespace kh2net
{
namespace
{
constexpr double kConnectRetry = 0.25;
constexpr double kConnectGiveUp = 30.0; // time for the host to let us in (hole punching)

void header(ByteWriter &w, PacketKind kind)
{
    w.u16(kMagic);
    w.u8(kProtocolVersion);
    w.u8(static_cast<uint8_t>(kind));
}
} // namespace

// STUN server addresses, looked up on a thread of their own (name lookups
// can take a while; the game loop must not wait).
struct Session::StunServers
{
    std::mutex mutex;
    std::vector<Address> list;
};

struct Session::Peer
{
    uint8_t id = 0;
    std::string name;
    Address address;
    uint32_t nonce = 0;
    Channel channel;
};

Session::Session()
{
    m_rng ^= static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

Session::~Session() { leave(); }

void Session::log(const std::string &text)
{
    if (onLog)
        onLog(text);
}

void Session::sendRaw(const Address &to, const std::vector<uint8_t> &packet)
{
    if (m_config.simulatedLoss > 0)
    {
        m_rng = m_rng * 1664525u + 1013904223u;
        if ((m_rng >> 8) * (1.0f / 16777216.0f) < m_config.simulatedLoss)
            return;
    }
    m_socket.send(to, packet.data(), packet.size());
}

bool Session::host(const SessionConfig &config)
{
    leave();
    m_config = config;
    if (!m_socket.open(config.port))
    {
        m_error = m_socket.error();
        m_status = Status::Failed;
        return false;
    }
    m_status = Status::Hosting;
    m_localId = 0;
    if (config.openPort)
        m_mapper.start(m_socket.localPort());
    startDiscovery();
    m_roster = {RosterEntry{0, config.name}};
    log("hosting on port " + std::to_string(m_socket.localPort()));
    return true;
}

bool Session::join(const SessionConfig &config, const Address &hostAddress)
{
    leave();
    m_config = config;
    if (!m_socket.open(config.port == kDefaultPort ? 0 : config.port))
    {
        m_error = m_socket.error();
        m_status = Status::Failed;
        return false;
    }
    m_rng = m_rng * 1664525u + 1013904223u;
    m_nonce = m_rng | 1u;
    m_hostAddress = hostAddress;
    m_status = Status::Connecting;
    startDiscovery();
    m_connectStart = -1; // set on the first update
    m_lastConnectSend = -1e9;
    log("joining " + hostAddress.str());
    return true;
}

void Session::leave()
{
    if (m_socket.isOpen())
    {
        std::vector<uint8_t> bye;
        ByteWriter w(bye);
        header(w, PacketKind::Disconnect);
        w.u8(m_localId);
        for (const auto &p : m_peers)
            m_socket.send(p->address, bye.data(), bye.size());
    }
    m_peers.clear();
    m_roster.clear();
    m_socket.close();
    m_mapper.stop();
    m_stunServers.reset();
    m_public = Address{};
    m_invites.clear();
    if (m_status != Status::Failed)
        m_status = Status::Offline;
}

std::vector<PeerInfo> Session::roster() const
{
    std::vector<PeerInfo> out;
    for (const RosterEntry &e : m_roster)
    {
        PeerInfo info;
        info.id = e.id;
        info.name = e.name;
        info.local = e.id == m_localId;
        for (const auto &p : m_peers)
            if (p->id == e.id || (!isHost() && e.id == 0 && p->id == 0))
            {
                info.rttMs = p->channel.rtt() * 1000.0;
                info.loss = p->channel.loss();
            }
        out.push_back(info);
    }
    return out;
}

void Session::sendState(const PlayerState &state)
{
    if (!active())
        return;
    PlayerState s = state;
    s.origin = m_localId;
    std::vector<uint8_t> body;
    ByteWriter w(body);
    writePlayerState(w, s);
    for (auto &p : m_peers)
        p->channel.queue(static_cast<uint8_t>(MsgType::PlayerState), body);
}

void Session::sendEnemies(const EnemyStates &states)
{
    if (!isHost())
        return;
    EnemyStates s = states;
    s.origin = m_localId;
    std::vector<uint8_t> body;
    ByteWriter w(body);
    writeEnemyStates(w, s);
    for (auto &p : m_peers)
        p->channel.queue(static_cast<uint8_t>(MsgType::EnemyStates), body);
}

void Session::sendEvent(uint16_t type, const std::vector<uint8_t> &data)
{
    if (!active())
        return;
    std::vector<uint8_t> body;
    ByteWriter w(body);
    w.u8(m_localId);
    w.u16(type);
    w.raw(data.data(), data.size());
    for (auto &p : m_peers)
        p->channel.queue(static_cast<uint8_t>(MsgType::GameEvent), body);
}

void Session::broadcastRoster()
{
    std::vector<uint8_t> body;
    ByteWriter w(body);
    w.u8(static_cast<uint8_t>(m_roster.size()));
    for (const RosterEntry &e : m_roster)
    {
        w.u8(e.id);
        w.str(e.name);
    }
    for (auto &p : m_peers)
        p->channel.queue(static_cast<uint8_t>(MsgType::Roster), body);
}

void Session::dropPeer(size_t index, const char *why)
{
    const uint8_t id = m_peers[index]->id;
    const std::string name = m_peers[index]->name;
    log(name + " left (" + why + ")");
    m_peers.erase(m_peers.begin() + static_cast<std::ptrdiff_t>(index));
    if (isHost())
    {
        m_roster.erase(std::remove_if(m_roster.begin(), m_roster.end(), [&](const RosterEntry &e) { return e.id == id; }),
                       m_roster.end());
        broadcastRoster();
        if (onPeer)
            onPeer(id, name, false);
    }
    else
    {
        m_status = Status::Failed;
        m_error = std::string("lost the host (") + why + ")";
        m_roster.clear();
    }
}

void Session::update(double now)
{
    m_now = now;
    if (!m_socket.isOpen())
        return;
    receiveAll(now);
    stunStep(now);
    for (size_t i = m_invites.size(); i-- > 0;)
    {
        Invite &inv = m_invites[i];
        if (now > inv.until || std::any_of(m_peers.begin(), m_peers.end(), [&](const auto &p) { return p->address == inv.address; }))
        {
            m_invites.erase(m_invites.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (now >= inv.next)
        {
            inv.next = now + 0.25;
            std::vector<uint8_t> pkt;
            ByteWriter w(pkt);
            header(w, PacketKind::Punch);
            sendRaw(inv.address, pkt);
        }
    }

    if (m_status == Status::Connecting)
    {
        if (m_connectStart < 0)
            m_connectStart = now;
        if (now - m_connectStart > kConnectGiveUp)
        {
            m_status = Status::Failed;
            m_error = "no answer from " + m_hostAddress.str();
            m_socket.close();
            return;
        }
        if (now - m_lastConnectSend >= kConnectRetry)
        {
            m_lastConnectSend = now;
            std::vector<uint8_t> pkt;
            ByteWriter w(pkt);
            header(w, PacketKind::Connect);
            w.u32(m_config.gameCrc);
            w.u32(m_nonce);
            w.str(m_config.name);
            w.u8(m_config.preferredId);
            sendRaw(m_hostAddress, pkt);
        }
        return;
    }

    for (size_t i = m_peers.size(); i-- > 0;)
        if (now - m_peers[i]->channel.lastReceive() > m_config.timeout)
        {
            dropPeer(i, "timed out");
            if (!isHost())
                return;
        }

    std::vector<uint8_t> pkt;
    for (auto &p : m_peers)
        while (p->channel.buildPacket(m_localId, now, pkt))
        {
            sendRaw(p->address, pkt);
            // Another packet only if messages are still waiting for room.
            if (pkt.size() < kMaxPacket - 64)
                break;
        }
}

void Session::startDiscovery()
{
    m_public = Address{};
    m_stunTries = 0;
    m_stunNext = 0;
    m_stunServers.reset();
    if (!m_config.discover)
        return;
    auto servers = std::make_shared<StunServers>();
    m_stunServers = servers;
    std::thread([servers] {
        for (const std::string &name : stun::servers())
        {
            Address a;
            if (Address::parse(name, 3478, a))
            {
                std::lock_guard<std::mutex> lock(servers->mutex);
                servers->list.push_back(a);
            }
        }
    }).detach();
}

void Session::stunStep(double now)
{
    // Up to 3 tries with each server, half a second apart, until one answers.
    if (!m_stunServers || m_public.valid() || now < m_stunNext || m_stunTries >= 9)
        return;
    Address server;
    {
        std::lock_guard<std::mutex> lock(m_stunServers->mutex);
        if (m_stunServers->list.empty())
            return; // still looking them up
        server = m_stunServers->list[static_cast<size_t>(m_stunTries / 3) % m_stunServers->list.size()];
    }
    if (m_stunTries % 3 == 0)
        for (uint8_t &b : m_stunTx)
        {
            m_rng = m_rng * 1664525u + 1013904223u;
            b = static_cast<uint8_t>(m_rng >> 24);
        }
    ++m_stunTries;
    m_stunNext = now + 0.5;
    const std::vector<uint8_t> req = stun::bindingRequest(m_stunTx);
    m_socket.send(server, req.data(), req.size());
}

Address Session::shareAddress() const
{
    const PortMapper::Status pm = m_mapper.status();
    if (pm.state == PortMapper::State::Mapped && pm.external.ip != 0)
        return pm.external;
    if (pm.state == PortMapper::State::Mapped && m_public.valid())
        return Address{m_public.ip, pm.external.port}; // the router did not say its address
    return m_public;
}

void Session::invite(const Address &player)
{
    if (!isHost() || !player.valid())
        return;
    m_invites.push_back(Invite{player, m_now + 30.0, 0});
    log("letting in " + player.str() + " (30 s)");
}

void Session::receiveAll(double now)
{
    uint8_t buf[2048];
    Address from;
    for (int n; (n = m_socket.receive(from, buf, sizeof(buf))) >= 0;)
    {
        if (stun::looksLikeStun(buf, static_cast<size_t>(n)))
        {
            Address mapped;
            uint8_t tx[12];
            if (!m_public.valid() && stun::parseResponse(buf, static_cast<size_t>(n), mapped, tx) &&
                std::memcmp(tx, m_stunTx, sizeof(tx)) == 0)
            {
                m_public = mapped;
                log("the internet sees this session as " + mapped.str());
            }
            continue;
        }
        ByteReader r(buf, static_cast<size_t>(n));
        if (r.u16() != kMagic)
            continue;
        const uint8_t version = r.u8();
        const auto kind = static_cast<PacketKind>(r.u8());
        if (!r.ok())
            continue;
        if (version != kProtocolVersion)
        {
            if (isHost() && kind == PacketKind::Connect)
            {
                std::vector<uint8_t> pkt;
                ByteWriter w(pkt);
                header(w, PacketKind::Reject);
                w.u8(static_cast<uint8_t>(RejectReason::WrongVersion));
                sendRaw(from, pkt);
            }
            continue;
        }
        auto peerIt = std::find_if(m_peers.begin(), m_peers.end(), [&](const auto &p) { return p->address == from; });
        switch (kind)
        {
        case PacketKind::Connect:
            if (isHost())
                handleConnect(from, r, now);
            break;
        case PacketKind::Accept:
            if (m_status == Status::Connecting && from == m_hostAddress)
            {
                const uint8_t id = r.u8();
                const uint32_t nonce = r.u32();
                if (!r.ok() || nonce != m_nonce)
                    break;
                m_localId = id;
                auto host = std::make_unique<Peer>();
                host->id = 0;
                host->name = "host";
                host->address = from;
                host->channel.touch(now);
                m_peers.push_back(std::move(host));
                m_status = Status::Connected;
                log("joined as player " + std::to_string(id + 1));
            }
            break;
        case PacketKind::Reject:
            if (m_status == Status::Connecting && from == m_hostAddress)
            {
                const auto why = static_cast<RejectReason>(r.u8());
                m_status = Status::Failed;
                m_error = why == RejectReason::Full        ? "the session is full"
                          : why == RejectReason::WrongGame ? "the host runs a different game build"
                                                           : "the host runs a different version";
                m_socket.close();
                return;
            }
            break;
        case PacketKind::Data:
            if (peerIt != m_peers.end())
            {
                r.u8(); // sender id (the address identifies the peer)
                handleData(**peerIt, r, now);
            }
            break;
        case PacketKind::Punch:
            break; // it got here: that was the point
        case PacketKind::Disconnect:
            if (peerIt != m_peers.end())
            {
                dropPeer(static_cast<size_t>(peerIt - m_peers.begin()), "left");
                if (!isHost())
                    return;
            }
            break;
        }
    }
}

void Session::handleConnect(const Address &from, ByteReader &r, double now)
{
    const uint32_t crc = r.u32();
    const uint32_t nonce = r.u32();
    std::string name = r.str();
    if (!r.ok())
        return;
    const uint8_t preferred = r.u8(); // optional
    const bool havePreferred = r.ok() && preferred > 0 && preferred < kMaxPlayers;
    auto reply = [&](PacketKind kind, auto &&fill) {
        std::vector<uint8_t> pkt;
        ByteWriter w(pkt);
        header(w, kind);
        fill(w);
        sendRaw(from, pkt);
    };
    // A retry from someone already accepted: repeat the answer.
    for (const auto &p : m_peers)
        if (p->address == from)
        {
            if (p->nonce == nonce)
                reply(PacketKind::Accept, [&](ByteWriter &w) { w.u8(p->id); w.u32(nonce); w.u8(kMaxPlayers); });
            return;
        }
    if (crc != m_config.gameCrc)
    {
        reply(PacketKind::Reject, [](ByteWriter &w) { w.u8(static_cast<uint8_t>(RejectReason::WrongGame)); });
        return;
    }
    auto taken = [&](uint8_t candidate) {
        return std::any_of(m_peers.begin(), m_peers.end(), [&](const auto &p) { return p->id == candidate; });
    };
    // The number asked for (a local player's), if free; else the first free one.
    uint8_t id = 1;
    if (havePreferred && !taken(preferred))
        id = preferred;
    else
        while (id < kMaxPlayers && taken(id))
            ++id;
    if (id >= kMaxPlayers)
    {
        reply(PacketKind::Reject, [](ByteWriter &w) { w.u8(static_cast<uint8_t>(RejectReason::Full)); });
        return;
    }
    if (name.empty())
        name = "Player " + std::to_string(id + 1);
    auto peer = std::make_unique<Peer>();
    peer->id = id;
    peer->name = name;
    peer->address = from;
    peer->nonce = nonce;
    peer->channel.touch(now);
    m_peers.push_back(std::move(peer));
    reply(PacketKind::Accept, [&](ByteWriter &w) { w.u8(id); w.u32(nonce); w.u8(kMaxPlayers); });
    m_roster.push_back(RosterEntry{id, name});
    log(name + " joined from " + from.str());
    broadcastRoster();
    if (onPeer)
        onPeer(id, name, true);
}

void Session::handleData(Peer &peer, ByteReader &r, double now)
{
    std::vector<Message> messages;
    peer.channel.receive(r, now, messages);
    for (const Message &m : messages)
        deliver(peer, m.type, m.body);
}

void Session::deliver(Peer &from, uint8_t type, const std::vector<uint8_t> &body)
{
    switch (static_cast<MsgType>(type))
    {
    case MsgType::PlayerState:
    {
        ByteReader r(body.data(), body.size());
        PlayerState s;
        if (!readPlayerState(r, s))
            return;
        if (isHost())
        {
            s.origin = from.id; // the host vouches for who sent it
            std::vector<uint8_t> relay;
            ByteWriter w(relay);
            writePlayerState(w, s);
            for (auto &p : m_peers)
                if (p.get() != &from)
                    p->channel.queue(type, relay);
        }
        if (onState)
            onState(s);
        break;
    }
    case MsgType::EnemyStates:
    {
        if (isHost())
            return; // only the host's world counts
        ByteReader r(body.data(), body.size());
        EnemyStates s;
        if (readEnemyStates(r, s) && onEnemies)
            onEnemies(s);
        break;
    }
    case MsgType::GameEvent:
    {
        if (body.size() < 3)
            return;
        std::vector<uint8_t> relayed = body;
        if (isHost())
        {
            relayed[0] = from.id;
            for (auto &p : m_peers)
                if (p.get() != &from)
                    p->channel.queue(type, relayed);
        }
        uint16_t evType;
        std::memcpy(&evType, relayed.data() + 1, 2);
        if (onEvent)
            onEvent(relayed[0], evType, std::vector<uint8_t>(relayed.begin() + 3, relayed.end()));
        break;
    }
    case MsgType::Roster:
    {
        if (isHost())
            return;
        ByteReader r(body.data(), body.size());
        const uint8_t count = r.u8();
        std::vector<RosterEntry> roster;
        for (uint8_t i = 0; i < count && r.ok(); ++i)
        {
            RosterEntry e;
            e.id = r.u8();
            e.name = r.str();
            roster.push_back(e);
        }
        if (!r.ok())
            return;
        // Report joins and leaves against the previous roster.
        for (const RosterEntry &e : roster)
            if (std::none_of(m_roster.begin(), m_roster.end(), [&](const RosterEntry &o) { return o.id == e.id; }))
            {
                if (e.id == 0 && !m_peers.empty())
                    m_peers[0]->name = e.name;
                if (onPeer && e.id != m_localId)
                    onPeer(e.id, e.name, true);
            }
        for (const RosterEntry &o : m_roster)
            if (std::none_of(roster.begin(), roster.end(), [&](const RosterEntry &e) { return o.id == e.id; }) && onPeer)
                onPeer(o.id, o.name, false);
        m_roster = std::move(roster);
        break;
    }
    }
}
} // namespace kh2net
