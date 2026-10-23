// Getting through home routers, with no server of our own.
//
//  - Port mapping (host): the router is asked to forward the session's UDP
//    port to this machine, with NAT-PMP (RFC 6886: UDP to the gateway, port
//    5351) or UPnP IGD (SSDP discovery, then the WANIPConnection /
//    WANPPPConnection AddPortMapping SOAP call), whichever answers. The
//    mapping lasts two hours and is renewed while hosting, and is removed when
//    the session ends. This runs on a thread of its own (it waits on the
//    network), and works for most home routers that have UPnP or NAT-PMP on.
//  - STUN (RFC 5389 Binding): a public STUN server says which address and
//    port the internet sees this session's socket as. The session sends and
//    receives these on its own socket, so the answer is that socket's mapping:
//    the address to give the other players.
//  - Hole punching: when neither side's router lets the other in on its own,
//    each side sends to the other's public address at the same time (the
//    client's join attempts, the host's "Let in" packets), which opens both
//    routers for that pair for most home NATs. Session::invite.
//
// Routers that map every destination to a different port ("symmetric" NAT,
// some mobile and carrier-grade NAT) defeat hole punching; those need port
// forwarding on the other side, or a relay.
#pragma once

#include <atomic>
#include <memory>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "udp_socket.h"

namespace kh2net
{
namespace stun
{
constexpr uint32_t kMagicCookie = 0x2112A442u;
// A Binding Request with this transaction id (12 bytes).
std::vector<uint8_t> bindingRequest(const uint8_t (&transaction)[12]);
// A Binding Success Response? Then the address it reports (XOR-MAPPED-ADDRESS,
// or MAPPED-ADDRESS) and its transaction id.
bool parseResponse(const uint8_t *data, size_t size, Address &mapped, uint8_t (&transaction)[12]);
// A STUN message at all (first two bits 0, the magic cookie)?
bool looksLikeStun(const uint8_t *data, size_t size);
// Public servers tried in turn.
const std::vector<std::string> &servers();
} // namespace stun

class PortMapper
{
public:
    enum class State
    {
        Idle,
        Working,  // asking the router
        Mapped,   // the router forwards the port
        Failed,   // no router answered, or it refused
    };
    struct Status
    {
        State state = State::Idle;
        std::string method;     // "UPnP" or "NAT-PMP"
        Address external;       // the router's public address and the forwarded port, when known
        std::string detail;     // why it failed, or a warning
    };

    PortMapper() = default;
    ~PortMapper();
    PortMapper(const PortMapper &) = delete;
    PortMapper &operator=(const PortMapper &) = delete;

    // Ask the router to forward UDP `port` here (in the background).
    void start(uint16_t port);
    // Remove the mapping (in the background; returns at once).
    void stop();
    Status status() const;

    struct Shared; // the thread's state: it may outlive this object (stop() lets it finish alone)
private:
    std::shared_ptr<Shared> m_shared;
    std::thread m_thread;
};

// Testing (kh2recomp --nat-test): what the router and a STUN server say,
// without mapping anything. Blocking; prints a report.
std::string probe();
} // namespace kh2net
