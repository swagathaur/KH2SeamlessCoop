// One side of a connection to one peer: packet sequence numbers and acks
// (each Data packet acks the newest packet received plus the 32 before it),
// round-trip time, and reliable messages that are resent until a packet
// carrying them is acked, then delivered in order exactly once.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "protocol.h"

namespace kh2net
{
struct Message
{
    uint8_t type = 0;
    std::vector<uint8_t> body; // for reliable types, without the reliable id
};

class Channel
{
public:
    // Queue a message. Unreliable ones go out with the next packet only.
    void queue(uint8_t type, std::vector<uint8_t> body);

    // Build the next Data packet (header and messages); false when there is
    // nothing worth sending (no messages, and a keepalive is not due).
    bool buildPacket(uint8_t senderId, double now, std::vector<uint8_t> &packet);

    // Handle a Data packet's header and messages (already past the kind byte
    // and sender id). Delivered messages are appended to `out`, in order.
    void receive(ByteReader &r, double now, std::vector<Message> &out);

    double lastReceive() const { return m_lastReceive; }
    double rtt() const { return m_rtt; }
    // Fraction of our recent packets that were never acked.
    float loss() const { return m_loss; }
    size_t reliablePending() const { return m_reliableOut.size(); }
    void touch(double now) { m_lastReceive = now; }

private:
    struct SentPacket
    {
        uint16_t seq = 0;
        double time = 0;
        bool acked = false;
        bool valid = false;
        std::vector<uint16_t> reliableIds;
    };
    struct ReliableOut
    {
        uint8_t type = 0;
        std::vector<uint8_t> body;
        double lastSent = -1.0;
    };
    void onAck(uint16_t seq, double now);
    double resendDelay() const;

    uint16_t m_nextSeq = 0;
    uint16_t m_remoteSeq = 0;
    uint32_t m_remoteBits = 0; // bit n: remoteSeq-1-n received
    bool m_haveRemote = false;
    std::array<SentPacket, 256> m_sent{};
    double m_lastSend = -1e9;
    double m_lastReceive = 0;
    double m_rtt = 0.1;
    float m_loss = 0;
    bool m_ackPending = false;

    std::deque<Message> m_unreliableOut;
    uint16_t m_nextReliableId = 0;
    std::map<uint16_t, ReliableOut> m_reliableOut; // by id (wrap ignored: ids stay within a small window)
    uint16_t m_nextDeliver = 0;
    std::map<uint16_t, Message> m_reliableIn; // received early, waiting for the gap
};
} // namespace kh2net
