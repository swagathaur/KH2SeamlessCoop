// Minimal non-blocking UDP (IPv4) for the co-op session. Winsock on Windows,
// BSD sockets elsewhere.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace kh2net
{
struct Address
{
    uint32_t ip = 0;   // host byte order
    uint16_t port = 0; // host byte order
    bool operator==(const Address &o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const Address &o) const { return !(*this == o); }
    bool valid() const { return port != 0; }
    std::string str() const;
    // "host:port", "host" (with defaultPort) or "1.2.3.4:5"; resolves names.
    static bool parse(const std::string &text, uint16_t defaultPort, Address &out);
};

class UdpSocket
{
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket &) = delete;
    UdpSocket &operator=(const UdpSocket &) = delete;

    // port 0 = any free port. False on failure (see error()).
    bool open(uint16_t port);
    void close();
    bool isOpen() const;
    uint16_t localPort() const { return m_port; }
    bool send(const Address &to, const void *data, size_t size);
    // One datagram, or -1 when none is waiting.
    int receive(Address &from, void *buffer, size_t capacity);
    const std::string &error() const { return m_error; }

private:
    intptr_t m_fd = -1;
    uint16_t m_port = 0;
    std::string m_error;
};
} // namespace kh2net
