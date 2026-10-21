#include "channel.h"

#include <algorithm>

namespace kh2net
{
namespace
{
constexpr double kKeepalive = 0.25;  // seconds between packets when idle
constexpr double kAckDelay = 0.03;   // send a bare ack this soon after receiving
constexpr size_t kMaxReliableInFlight = 1024;
} // namespace

void Channel::queue(uint8_t type, std::vector<uint8_t> body)
{
    if (type & kReliableBit)
    {
        if (m_reliableOut.size() >= kMaxReliableInFlight)
            return; // peer is gone or hopelessly behind; the session times it out
        ReliableOut out;
        out.type = type;
        out.body = std::move(body);
        m_reliableOut.emplace(m_nextReliableId++, std::move(out));
    }
    else
    {
        // Newest state wins: drop an older queued message of the same type
        // from the same origin (first body byte) that never went out.
        const uint8_t origin = body.empty() ? 0 : body[0];
        m_unreliableOut.erase(std::remove_if(m_unreliableOut.begin(), m_unreliableOut.end(),
                                             [&](const Message &m) {
                                                 return m.type == type && !m.body.empty() && m.body[0] == origin;
                                             }),
                              m_unreliableOut.end());
        m_unreliableOut.push_back(Message{type, std::move(body)});
    }
}

double Channel::resendDelay() const { return std::clamp(m_rtt * 1.5, 0.05, 1.0); }

bool Channel::buildPacket(uint8_t senderId, double now, std::vector<uint8_t> &packet)
{
    const double delay = resendDelay();
    bool reliableDue = false;
    for (const auto &[id, r] : m_reliableOut)
        if (r.lastSent < 0 || now - r.lastSent >= delay)
        {
            reliableDue = true;
            break;
        }
    const bool idle = m_unreliableOut.empty() && !reliableDue;
    if (idle && !(m_ackPending && now - m_lastSend >= kAckDelay) && now - m_lastSend < kKeepalive)
        return false;

    packet.clear();
    ByteWriter w(packet);
    w.u16(kMagic);
    w.u8(kProtocolVersion);
    w.u8(static_cast<uint8_t>(PacketKind::Data));
    w.u8(senderId);
    const uint16_t seq = m_nextSeq++;
    w.u16(seq);
    w.u8(m_haveRemote ? 1 : 0); // acks valid: nothing received yet means nothing to ack
    w.u16(m_remoteSeq);
    w.u32(m_haveRemote ? m_remoteBits : 0);

    SentPacket &slot = m_sent[seq % m_sent.size()];
    if (slot.valid)
        m_loss = m_loss * 0.95f + (slot.acked ? 0.0f : 0.05f);
    slot = SentPacket{};
    slot.seq = seq;
    slot.time = now;
    slot.valid = true;

    for (auto &[id, r] : m_reliableOut)
    {
        if (!(r.lastSent < 0 || now - r.lastSent >= delay))
            continue;
        if (w.size() + 5 + r.body.size() > kMaxPacket)
            break;
        w.u8(r.type);
        w.u16(static_cast<uint16_t>(r.body.size() + 2));
        w.u16(id);
        w.raw(r.body.data(), r.body.size());
        r.lastSent = now;
        slot.reliableIds.push_back(id);
    }
    while (!m_unreliableOut.empty())
    {
        const Message &m = m_unreliableOut.front();
        if (w.size() + 3 + m.body.size() > kMaxPacket)
            break; // stays for the next packet
        w.u8(m.type);
        w.u16(static_cast<uint16_t>(m.body.size()));
        w.raw(m.body.data(), m.body.size());
        m_unreliableOut.pop_front();
    }
    m_lastSend = now;
    m_ackPending = false;
    return true;
}

void Channel::onAck(uint16_t seq, double now)
{
    SentPacket &slot = m_sent[seq % m_sent.size()];
    if (!slot.valid || slot.seq != seq || slot.acked)
        return;
    slot.acked = true;
    const double sample = now - slot.time;
    m_rtt += (sample - m_rtt) * 0.1;
    for (uint16_t id : slot.reliableIds)
        m_reliableOut.erase(id);
}

void Channel::receive(ByteReader &r, double now, std::vector<Message> &out)
{
    const uint16_t seq = r.u16();
    const uint8_t ackValid = r.u8();
    const uint16_t ack = r.u16();
    const uint32_t ackBits = r.u32();
    if (!r.ok())
        return;
    m_lastReceive = now;

    // Our packets the peer has seen.
    if (ackValid & 1)
    {
        onAck(ack, now);
        for (int i = 0; i < 32; ++i)
            if (ackBits & (1u << i))
                onAck(static_cast<uint16_t>(ack - 1 - i), now);
    }

    // Their packet: record it for our next ack.
    if (!m_haveRemote)
    {
        m_haveRemote = true;
        m_remoteSeq = seq;
        m_remoteBits = 0;
    }
    else if (seqNewer(seq, m_remoteSeq))
    {
        const uint16_t shift = static_cast<uint16_t>(seq - m_remoteSeq);
        if (shift < 32)
            m_remoteBits = (m_remoteBits << shift) | (1u << (shift - 1));
        else
            m_remoteBits = shift == 32 ? 1u << 31 : 0;
        m_remoteSeq = seq;
    }
    else if (seq != m_remoteSeq)
    {
        const uint16_t back = static_cast<uint16_t>(m_remoteSeq - seq);
        if (back <= 32)
        {
            if (m_remoteBits & (1u << (back - 1)))
                return; // duplicate packet
            m_remoteBits |= 1u << (back - 1);
        }
        else
            return; // too old to ack; its reliable messages will come again
    }
    else
        return; // duplicate of the newest
    m_ackPending = true;

    while (r.remaining() >= 3)
    {
        const uint8_t type = r.u8();
        const uint16_t len = r.u16();
        if (!r.ok() || r.remaining() < len)
            return;
        const uint8_t *body = r.cursor();
        r.skip(len);
        if (type & kReliableBit)
        {
            if (len < 2)
                continue;
            uint16_t id16;
            std::memcpy(&id16, body, 2);
            const int16_t ahead = static_cast<int16_t>(static_cast<uint16_t>(id16 - m_nextDeliver));
            if (ahead < 0)
                continue; // delivered already; the peer missed our ack
            const uint16_t id = id16;
            if (!m_reliableIn.count(id))
                m_reliableIn.emplace(id, Message{type, std::vector<uint8_t>(body + 2, body + len)});
        }
        else
        {
            out.push_back(Message{type, std::vector<uint8_t>(body, body + len)});
        }
    }
    // Deliver reliable messages that are next in order.
    for (;;)
    {
        auto it = m_reliableIn.find(m_nextDeliver);
        if (it == m_reliableIn.end())
            break;
        out.push_back(std::move(it->second));
        m_reliableIn.erase(it);
        ++m_nextDeliver;
    }
}
} // namespace kh2net
