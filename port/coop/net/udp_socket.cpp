#include "udp_socket.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kh2net
{
namespace
{
#if defined(_WIN32)
bool startup()
{
    static const bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
int lastError() { return WSAGetLastError(); }
bool wouldBlock(int e) { return e == WSAEWOULDBLOCK || e == WSAECONNRESET; }
#else
bool startup() { return true; }
int lastError() { return errno; }
bool wouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == ECONNREFUSED; }
#endif

sockaddr_in toSockaddr(const Address &a)
{
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(a.ip);
    sa.sin_port = htons(a.port);
    return sa;
}
} // namespace

std::string Address::str() const
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255,
                  port);
    return buf;
}

bool Address::parse(const std::string &text, uint16_t defaultPort, Address &out)
{
    if (!startup())
        return false;
    std::string host = text;
    uint16_t port = defaultPort;
    const size_t colon = text.rfind(':');
    if (colon != std::string::npos)
    {
        host = text.substr(0, colon);
        const unsigned long p = std::strtoul(text.c_str() + colon + 1, nullptr, 10);
        if (p == 0 || p > 65535)
            return false;
        port = static_cast<uint16_t>(p);
    }
    if (host.empty())
        host = "127.0.0.1";
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
        return false;
    out.ip = ntohl(reinterpret_cast<sockaddr_in *>(res->ai_addr)->sin_addr.s_addr);
    out.port = port;
    freeaddrinfo(res);
    return true;
}

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::isOpen() const { return m_fd != -1; }

bool UdpSocket::open(uint16_t port)
{
    close();
    if (!startup())
    {
        m_error = "network startup failed";
        return false;
    }
    const auto fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#if defined(_WIN32)
    if (fd == INVALID_SOCKET)
#else
    if (fd < 0)
#endif
    {
        m_error = "socket() failed: " + std::to_string(lastError());
        return false;
    }
    m_fd = static_cast<intptr_t>(fd);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) != 0)
    {
        m_error = "port " + std::to_string(port) + " is in use or not allowed (" + std::to_string(lastError()) + ")";
        close();
        return false;
    }
#if defined(_WIN32)
    u_long nonBlocking = 1;
    ioctlsocket(fd, FIONBIO, &nonBlocking);
    // A peer that went away makes Windows report WSAECONNRESET on the next
    // receive; turn that off so one dead peer does not disturb the others.
    BOOL off = FALSE;
    DWORD bytes = 0;
    WSAIoctl(fd, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof(off), nullptr, 0, &bytes, nullptr,
             nullptr);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
    socklen_t len = sizeof(sa);
    getsockname(fd, reinterpret_cast<sockaddr *>(&sa), &len);
    m_port = ntohs(sa.sin_port);
    return true;
}

void UdpSocket::close()
{
    if (m_fd == -1)
        return;
#if defined(_WIN32)
    closesocket(static_cast<SOCKET>(m_fd));
#else
    ::close(static_cast<int>(m_fd));
#endif
    m_fd = -1;
    m_port = 0;
}

bool UdpSocket::send(const Address &to, const void *data, size_t size)
{
    if (m_fd == -1)
        return false;
    const sockaddr_in sa = toSockaddr(to);
    const auto sent = ::sendto(static_cast<decltype(::socket(0, 0, 0))>(m_fd), static_cast<const char *>(data),
                               static_cast<int>(size), 0, reinterpret_cast<const sockaddr *>(&sa), sizeof(sa));
    return sent >= 0 && static_cast<size_t>(sent) == size;
}

int UdpSocket::receive(Address &from, void *buffer, size_t capacity)
{
    if (m_fd == -1)
        return -1;
    for (;;)
    {
        sockaddr_in sa{};
        socklen_t len = sizeof(sa);
        const auto got = ::recvfrom(static_cast<decltype(::socket(0, 0, 0))>(m_fd), static_cast<char *>(buffer),
                                    static_cast<int>(capacity), 0, reinterpret_cast<sockaddr *>(&sa), &len);
        if (got < 0)
        {
            if (wouldBlock(lastError()))
            {
#if defined(_WIN32)
                if (lastError() == WSAECONNRESET)
                    continue;
#else
                if (lastError() == ECONNREFUSED)
                    continue;
#endif
                return -1;
            }
            return -1;
        }
        from.ip = ntohl(sa.sin_addr.s_addr);
        from.port = ntohs(sa.sin_port);
        return static_cast<int>(got);
    }
}
} // namespace kh2net
