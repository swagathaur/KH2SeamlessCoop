// Co-op wire format. Little-endian, no padding; every packet starts with the
// magic, protocol version and kind, so strangers' datagrams and other
// versions are dropped at the door.
//
//   Connect   game crc u32, nonce u32, name (u8 length + bytes)
//   Accept    player id u8, nonce u32, max players u8
//   Reject    reason u8
//   Data      sender u8, seq u16, ack flags u8 (1 = acks valid), ack u16,
//             ack bits u32, then messages:
//               type u8, length u16, body; types with kReliableBit set start
//               their body with a u16 reliable id
//
// Message bodies: PlayerState (writePlayerState), EnemyStates
// (writeEnemyStates; host only), Roster, GameEvent (origin u8, type u16,
// data; GameEventType below).
//   Disconnect sender u8
//   Punch      (nothing): hole punching, see nat.h
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace kh2net
{
constexpr uint16_t kMagic = 0x324B; // "K2" on the wire
constexpr uint8_t kProtocolVersion = 5;
constexpr size_t kMaxPacket = 1200; // stays under every path MTU
constexpr uint8_t kMaxPlayers = 3;
constexpr uint16_t kDefaultPort = 27552;

enum class PacketKind : uint8_t
{
    Connect = 1,
    Accept = 2,
    Reject = 3,
    Data = 4,
    Disconnect = 5,
    Punch = 6, // no body: opens the sender's router toward us (hole punching); ignored
};

enum class RejectReason : uint8_t
{
    Full = 1,
    WrongGame = 2,
    WrongVersion = 3,
};

constexpr uint8_t kReliableBit = 0x80;
enum class MsgType : uint8_t
{
    PlayerState = 1,                 // unreliable, newest wins
    Roster = 2 | kReliableBit,       // host -> clients: who is in the session
    GameEvent = 3 | kReliableBit,    // reliable, ordered, relayed by the host
    EnemyStates = 4,                 // unreliable, host -> clients, newest wins
};

// GameEvent types.
enum class GameEventType : uint16_t
{
    EnemyDamage = 1, // client -> host: world u8, room u8, serial i16, amount i32
    PartnerHeal = 2, // to everyone: target player u8, hp i32, mp i32 (your partner here healed you)
    Loadout = 3,     // to everyone: the sender's character save record (0x114 bytes)
    EnemyHit = 4,    // client -> host: world u8, room u8, serial i16, DAMAGE (0x60), its attack parameters (0x80)
    StoryChunk = 5,  // host -> clients: the host's story (story.h), all of it or what changed
    Scene = 6,       // host -> clients: world u8, room u8, door u16, map u16, battle u16, event u16, cutscene u8
    SkipVote = 7,    // client -> host: this player asks to skip the cutscene playing
};

// Where a player is and what they are doing, sent ~30 times a second.
struct PlayerState
{
    uint8_t origin = 0;   // player id
    uint32_t tick = 0;    // sender's frame counter
    uint8_t world = 0, room = 0;
    uint16_t door = 0;  // the entrance they came in by
    float pos[3] = {0, 0, 0};
    float rotY = 0;
    uint16_t model = 0; // objentry id of their character
    uint16_t anim = 0;  // motion index
    float animTime = 0;
    int32_t hp = 0, maxHp = 0;
    int32_t mp = 0, maxMp = 0;
    uint16_t weapon = 0; // objentry id of their weapon object
    uint8_t flags = 0;
};

// The enemies in the host's room, sent ~20 times a second. Each is known by
// its spawn serial in the room's data (the same on every machine).
struct EnemyState
{
    int16_t serial = 0;
    uint16_t entry = 0; // objentry id
    float pos[3] = {0, 0, 0};
    float yaw = 0;
    uint16_t motion = 0;
    int32_t hp = 0, maxHp = 0;
};
struct EnemyStates
{
    uint8_t origin = 0;
    uint32_t tick = 0;
    uint8_t world = 0, room = 0;
    std::vector<EnemyState> enemies; // at most kMaxEnemies
};
constexpr size_t kMaxEnemies = 32;

struct RosterEntry
{
    uint8_t id = 0;
    std::string name;
};

class ByteWriter
{
public:
    explicit ByteWriter(std::vector<uint8_t> &out) : m_out(out) {}
    void u8(uint8_t v) { m_out.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void i32(int32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void str(const std::string &s)
    {
        const size_t n = s.size() < 255 ? s.size() : 255;
        u8(static_cast<uint8_t>(n));
        raw(s.data(), n);
    }
    void raw(const void *p, size_t n)
    {
        const auto *b = static_cast<const uint8_t *>(p);
        m_out.insert(m_out.end(), b, b + n); // little-endian hosts only (x86, ARM)
    }
    size_t size() const { return m_out.size(); }

private:
    std::vector<uint8_t> &m_out;
};

class ByteReader
{
public:
    ByteReader(const uint8_t *p, size_t n) : m_p(p), m_end(p + n) {}
    bool ok() const { return m_ok; }
    size_t remaining() const { return size_t(m_end - m_p); }
    const uint8_t *cursor() const { return m_p; }
    uint8_t u8() { uint8_t v = 0; raw(&v, 1); return v; }
    uint16_t u16() { uint16_t v = 0; raw(&v, 2); return v; }
    uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
    int32_t i32() { int32_t v = 0; raw(&v, 4); return v; }
    float f32() { float v = 0; raw(&v, 4); return v; }
    std::string str()
    {
        const uint8_t n = u8();
        if (!m_ok || remaining() < n) { m_ok = false; return {}; }
        std::string s(reinterpret_cast<const char *>(m_p), n);
        m_p += n;
        return s;
    }
    void skip(size_t n)
    {
        if (remaining() < n) { m_ok = false; m_p = m_end; return; }
        m_p += n;
    }
    void raw(void *dst, size_t n)
    {
        if (remaining() < n) { m_ok = false; m_p = m_end; std::memset(dst, 0, n); return; }
        std::memcpy(dst, m_p, n);
        m_p += n;
    }

private:
    const uint8_t *m_p;
    const uint8_t *m_end;
    bool m_ok = true;
};

void writePlayerState(ByteWriter &w, const PlayerState &s);
bool readPlayerState(ByteReader &r, PlayerState &s);
void writeEnemyStates(ByteWriter &w, const EnemyStates &s);
bool readEnemyStates(ByteReader &r, EnemyStates &s);

// Sequence numbers wrap; a is newer than b.
inline bool seqNewer(uint16_t a, uint16_t b)
{
    return static_cast<int16_t>(static_cast<uint16_t>(a - b)) > 0;
}
} // namespace kh2net
