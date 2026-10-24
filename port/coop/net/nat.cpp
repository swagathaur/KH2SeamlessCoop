#include "nat.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
using socklen_t = int;
using sock_t = SOCKET;
constexpr sock_t kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
constexpr sock_t kNoSocket = -1;
#endif

namespace kh2net
{
namespace
{
using Clock = std::chrono::steady_clock;

bool netStartup()
{
#if defined(_WIN32)
    static const bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

// A socket that closes itself.
struct Sock
{
    sock_t fd = kNoSocket;
    Sock(int type, int proto) { fd = netStartup() ? ::socket(AF_INET, type, proto) : kNoSocket; }
    ~Sock()
    {
        if (fd == kNoSocket)
            return;
#if defined(_WIN32)
        closesocket(fd);
#else
        ::close(fd);
#endif
    }
    Sock(const Sock &) = delete;
    Sock &operator=(const Sock &) = delete;
    bool ok() const { return fd != kNoSocket; }
};

sockaddr_in sockaddrOf(uint32_t ip, uint16_t port)
{
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(ip);
    sa.sin_port = htons(port);
    return sa;
}

// Wait until the socket can be read (or written), up to `ms`.
bool waitFor(sock_t fd, int ms, bool write = false)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    const int n = ::select(static_cast<int>(fd + 1), write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv);
    return n > 0;
}

void setNonBlocking(sock_t fd, bool on)
{
#if defined(_WIN32)
    u_long v = on ? 1 : 0;
    ioctlsocket(fd, FIONBIO, &v);
#else
    const int f = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
#endif
}

std::string ipStr(uint32_t ip) { return Address{ip, 1}.str().substr(0, Address{ip, 1}.str().rfind(':')); }

bool privateIp(uint32_t ip)
{
    return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 22) == (100u << 2 | 1) ||
           (ip >> 24) == 127 || ip == 0;
}

// This machine's address on the way to `toward` (no packet is sent).
uint32_t localAddressToward(uint32_t toward)
{
    Sock s(SOCK_DGRAM, IPPROTO_UDP);
    if (!s.ok())
        return 0;
    sockaddr_in sa = sockaddrOf(toward, 9);
    if (::connect(s.fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) != 0)
        return 0;
    socklen_t len = sizeof(sa);
    if (getsockname(s.fd, reinterpret_cast<sockaddr *>(&sa), &len) != 0)
        return 0;
    return ntohl(sa.sin_addr.s_addr);
}

// The default gateway (the router).
uint32_t defaultGateway()
{
#if defined(_WIN32)
    MIB_IPFORWARDROW row{};
    if (GetBestRoute(inet_addr("8.8.8.8"), 0, &row) == NO_ERROR && row.dwForwardNextHop)
        return ntohl(row.dwForwardNextHop);
#else
    std::ifstream f("/proc/net/route");
    std::string iface, dest, gw;
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line))
    {
        char i[64];
        unsigned d, g, flags;
        if (std::sscanf(line.c_str(), "%63s %x %x %x", i, &d, &g, &flags) == 4 && d == 0 && g != 0)
            return ntohl(g);
    }
#endif
    // A common guess: x.y.z.1 on our own network.
    const uint32_t local = localAddressToward(0x08080808u);
    return local ? (local & 0xFFFFFF00u) | 1u : 0;
}

// ---- NAT-PMP (RFC 6886) ----

// Request/response with the gateway on port 5351: retries 250, 500, 1000 ms.
bool natpmpCall(uint32_t gateway, const std::vector<uint8_t> &req, std::vector<uint8_t> &resp, uint8_t wantOp)
{
    Sock s(SOCK_DGRAM, IPPROTO_UDP);
    if (!s.ok())
        return false;
    const sockaddr_in to = sockaddrOf(gateway, 5351);
    for (int wait = 250; wait <= 1000; wait *= 2)
    {
        ::sendto(s.fd, reinterpret_cast<const char *>(req.data()), static_cast<int>(req.size()), 0,
                 reinterpret_cast<const sockaddr *>(&to), sizeof(to));
        const auto until = Clock::now() + std::chrono::milliseconds(wait);
        while (Clock::now() < until)
        {
            const int left = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count());
            if (!waitFor(s.fd, std::max(left, 1)))
                break;
            uint8_t buf[64];
            sockaddr_in from{};
            socklen_t len = sizeof(from);
            const int n = static_cast<int>(::recvfrom(s.fd, reinterpret_cast<char *>(buf), sizeof(buf), 0,
                                                      reinterpret_cast<sockaddr *>(&from), &len));
            if (n >= 8 && ntohl(from.sin_addr.s_addr) == gateway && buf[0] == 0 && buf[1] == wantOp)
            {
                resp.assign(buf, buf + n);
                return true;
            }
        }
    }
    return false;
}

uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool natpmpExternal(uint32_t gateway, uint32_t &ip)
{
    std::vector<uint8_t> resp;
    if (!natpmpCall(gateway, {0, 0}, resp, 128) || resp.size() < 12 || be16(&resp[2]) != 0)
        return false;
    ip = be32(&resp[8]);
    return true;
}

// Map UDP `port` (lifetime 0 removes it). Returns the public port, or 0.
uint16_t natpmpMap(uint32_t gateway, uint16_t port, uint32_t lifetime, std::string &err)
{
    std::vector<uint8_t> req = {0,
                                1,
                                0,
                                0,
                                static_cast<uint8_t>(port >> 8),
                                static_cast<uint8_t>(port),
                                static_cast<uint8_t>(lifetime ? port >> 8 : 0),
                                static_cast<uint8_t>(lifetime ? port : 0),
                                static_cast<uint8_t>(lifetime >> 24),
                                static_cast<uint8_t>(lifetime >> 16),
                                static_cast<uint8_t>(lifetime >> 8),
                                static_cast<uint8_t>(lifetime)};
    std::vector<uint8_t> resp;
    if (!natpmpCall(gateway, req, resp, 129) || resp.size() < 16)
    {
        err = "no NAT-PMP answer";
        return 0;
    }
    if (const uint16_t result = be16(&resp[2]))
    {
        err = "NAT-PMP refused (" + std::to_string(result) + ")";
        return 0;
    }
    return lifetime ? be16(&resp[10]) : port;
}

// ---- UPnP IGD ----

struct Url
{
    uint32_t ip = 0;
    uint16_t port = 80;
    std::string path = "/";
};
bool parseUrl(const std::string &u, Url &out)
{
    const std::string p = "http://";
    if (u.compare(0, p.size(), p) != 0)
        return false;
    const size_t start = p.size();
    const size_t slash = u.find('/', start);
    const std::string hostport = u.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    out.path = slash == std::string::npos ? "/" : u.substr(slash);
    Address a;
    if (!Address::parse(hostport, 80, a))
        return false;
    out.ip = a.ip;
    out.port = a.port;
    return true;
}

std::string lower(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The text of the first <tag>...</tag> in `xml` at or after `from` (any namespace prefix).
std::string xmlTag(const std::string &xml, const std::string &tag, size_t from = 0, size_t *end = nullptr)
{
    const std::string l = lower(xml), t = lower(tag);
    size_t i = from;
    while ((i = l.find(t, i)) != std::string::npos)
    {
        // "<tag>" or "<prefix:tag>"
        const size_t lt = l.rfind('<', i);
        const size_t gt = l.find('>', i);
        if (lt == std::string::npos || gt == std::string::npos || l[lt + 1] == '/' ||
            (l.compare(i + t.size(), 1, ">") != 0 && l[i + t.size()] != ' '))
        {
            i += t.size();
            continue;
        }
        const size_t close = l.find("</", gt);
        if (close == std::string::npos)
            return {};
        if (end)
            *end = close;
        std::string v = xml.substr(gt + 1, close - gt - 1);
        v.erase(0, v.find_first_not_of(" \t\r\n"));
        v.erase(v.find_last_not_of(" \t\r\n") + 1);
        return v;
    }
    return {};
}

// A small blocking HTTP/1.1 client (Connection: close; chunked bodies decoded).
bool httpRequest(const Url &url, const std::string &method, const std::string &extraHeaders, const std::string &body,
                 int &status, std::string &response, int timeoutMs = 3000)
{
    Sock s(SOCK_STREAM, IPPROTO_TCP);
    if (!s.ok())
        return false;
    setNonBlocking(s.fd, true);
    sockaddr_in sa = sockaddrOf(url.ip, url.port);
    ::connect(s.fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa));
    if (!waitFor(s.fd, timeoutMs, true))
        return false;
    int soErr = 0;
    socklen_t len = sizeof(soErr);
    getsockopt(s.fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soErr), &len);
    if (soErr != 0)
        return false;
    std::string req = method + " " + url.path + " HTTP/1.1\r\nHost: " + ipStr(url.ip) + ":" + std::to_string(url.port) +
                      "\r\nConnection: close\r\n" + extraHeaders;
    if (!body.empty() || method == "POST")
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    size_t sent = 0;
    while (sent < req.size())
    {
        if (!waitFor(s.fd, timeoutMs, true))
            return false;
        const int n = static_cast<int>(::send(s.fd, req.data() + sent, static_cast<int>(req.size() - sent), 0));
        if (n <= 0)
            return false;
        sent += static_cast<size_t>(n);
    }
    std::string raw;
    const auto until = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (Clock::now() < until && raw.size() < (1u << 20))
    {
        if (!waitFor(s.fd, 200))
            continue;
        char buf[4096];
        const int n = static_cast<int>(::recv(s.fd, buf, sizeof(buf), 0));
        if (n <= 0)
            break;
        raw.append(buf, static_cast<size_t>(n));
    }
    const size_t headEnd = raw.find("\r\n\r\n");
    if (raw.compare(0, 5, "HTTP/") != 0 || headEnd == std::string::npos)
        return false;
    status = std::atoi(raw.c_str() + raw.find(' ') + 1);
    const std::string head = lower(raw.substr(0, headEnd));
    std::string b = raw.substr(headEnd + 4);
    if (head.find("transfer-encoding: chunked") != std::string::npos)
    {
        std::string out;
        size_t i = 0;
        while (i < b.size())
        {
            const size_t eol = b.find("\r\n", i);
            if (eol == std::string::npos)
                break;
            const unsigned long n = std::strtoul(b.c_str() + i, nullptr, 16);
            if (n == 0)
                break;
            out += b.substr(eol + 2, n);
            i = eol + 2 + n + 2;
        }
        b = out;
    }
    response = b;
    return true;
}

struct Igd
{
    Url control;
    std::string service; // e.g. urn:schemas-upnp-org:service:WANIPConnection:1
};

// SSDP: the routers' description URLs.
std::vector<std::string> ssdpDiscover(int waitMs = 2000)
{
    std::vector<std::string> found;
    Sock s(SOCK_DGRAM, IPPROTO_UDP);
    if (!s.ok())
        return found;
    const int ttl = 2;
    setsockopt(s.fd, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char *>(&ttl), sizeof(ttl));
    const sockaddr_in to = sockaddrOf(0xEFFFFFFAu, 1900); // 239.255.255.250
    for (const char *st : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                           "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                           "urn:schemas-upnp-org:service:WANIPConnection:1",
                           "urn:schemas-upnp-org:service:WANPPPConnection:1"})
    {
        const std::string msg = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                                            "MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") +
                                st + "\r\n\r\n";
        ::sendto(s.fd, msg.data(), static_cast<int>(msg.size()), 0, reinterpret_cast<const sockaddr *>(&to),
                 sizeof(to));
    }
    const auto until = Clock::now() + std::chrono::milliseconds(waitMs);
    while (Clock::now() < until)
    {
        const int left =
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count());
        if (!waitFor(s.fd, std::max(left, 1)))
            break;
        char buf[2048];
        const int n = static_cast<int>(::recv(s.fd, buf, sizeof(buf) - 1, 0));
        if (n <= 0)
            continue;
        const std::string reply(buf, static_cast<size_t>(n));
        const std::string l = lower(reply);
        const size_t at = l.find("\nlocation:");
        if (at == std::string::npos)
            continue;
        const size_t eol = reply.find("\r\n", at + 1);
        std::string loc = reply.substr(at + 10, eol == std::string::npos ? std::string::npos : eol - at - 10);
        loc.erase(0, loc.find_first_not_of(" \t"));
        loc.erase(loc.find_last_not_of(" \t\r\n") + 1);
        if (std::find(found.begin(), found.end(), loc) == found.end())
            found.push_back(loc);
    }
    return found;
}

// The WAN connection service of the first router description that has one.
bool findIgd(Igd &out, std::string &err)
{
    const std::vector<std::string> locations = ssdpDiscover();
    if (locations.empty())
    {
        err = "no UPnP router answered";
        return false;
    }
    for (const std::string &loc : locations)
    {
        Url url;
        int status = 0;
        std::string xml;
        if (!parseUrl(loc, url) || !httpRequest(url, "GET", "", "", status, xml) || status != 200)
            continue;
        const std::string base = xmlTag(xml, "URLBase");
        const std::string l = lower(xml);
        for (const char *want : {"wanipconnection:2", "wanipconnection:1", "wanpppconnection:1"})
        {
            size_t i = 0;
            while ((i = l.find(want, i)) != std::string::npos)
            {
                // The <service> block this serviceType is in.
                const size_t start = l.rfind("<service>", i);
                const size_t stop = l.find("</service>", i);
                if (start == std::string::npos || stop == std::string::npos)
                    break;
                const std::string block = xml.substr(start, stop - start);
                const std::string type = xmlTag(block, "serviceType");
                std::string control = xmlTag(block, "controlURL");
                i = stop;
                if (type.empty() || control.empty())
                    continue;
                Url c;
                if (control.compare(0, 7, "http://") == 0)
                {
                    if (!parseUrl(control, c))
                        continue;
                }
                else
                {
                    Url b = url;
                    if (!base.empty())
                        parseUrl(base, b);
                    c = b;
                    c.path = control[0] == '/' ? control : "/" + control;
                }
                out.control = c;
                out.service = type;
                return true;
            }
        }
    }
    err = "the UPnP device is not a router (no WAN connection service)";
    return false;
}

bool soap(const Igd &igd, const std::string &action, const std::string &args, std::string &response, std::string &err)
{
    const std::string body = "<?xml version=\"1.0\"?>\r\n"
                             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                             "<u:" + action + " xmlns:u=\"" + igd.service + "\">" + args + "</u:" + action +
                             "></s:Body></s:Envelope>\r\n";
    const std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + igd.service + "#" +
                                action + "\"\r\n";
    int status = 0;
    if (!httpRequest(igd.control, "POST", headers, body, status, response))
    {
        err = "the router did not answer " + action;
        return false;
    }
    if (status != 200)
    {
        const std::string code = xmlTag(response, "errorCode"), desc = xmlTag(response, "errorDescription");
        err = action + " refused (" + (code.empty() ? std::to_string(status) : code) +
              (desc.empty() ? "" : " " + desc) + ")";
        return false;
    }
    return true;
}

bool upnpExternal(const Igd &igd, uint32_t &ip, std::string &err)
{
    std::string resp;
    if (!soap(igd, "GetExternalIPAddress", "", resp, err))
        return false;
    Address a;
    if (!Address::parse(xmlTag(resp, "NewExternalIPAddress") + ":1", 1, a))
    {
        err = "the router gave no external address";
        return false;
    }
    ip = a.ip;
    return true;
}

bool upnpMap(const Igd &igd, uint16_t port, uint32_t localIp, uint32_t lease, std::string &err)
{
    auto args = [&](uint32_t l) {
        return "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
               "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + std::to_string(port) +
               "</NewInternalPort><NewInternalClient>" + ipStr(localIp) +
               "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>KH2 co-op"
               "</NewPortMappingDescription><NewLeaseDuration>" +
               std::to_string(l) + "</NewLeaseDuration>";
    };
    std::string resp;
    if (soap(igd, "AddPortMapping", args(lease), resp, err))
    {
        err.clear();
        return true;
    }
    // 725: only permanent leases (we remove it ourselves when done).
    if (err.find("725") != std::string::npos && soap(igd, "AddPortMapping", args(0), resp, err))
    {
        err.clear();
        return true;
    }
    return false;
}

void upnpUnmap(const Igd &igd, uint16_t port)
{
    std::string resp, err;
    soap(igd,
         "DeletePortMapping",
         "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
             "</NewExternalPort><NewProtocol>UDP</NewProtocol>",
         resp, err);
}

} // namespace

// ---- STUN ----

namespace stun
{
std::vector<uint8_t> bindingRequest(const uint8_t (&transaction)[12])
{
    std::vector<uint8_t> m = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    m.insert(m.end(), transaction, transaction + 12);
    return m;
}

bool looksLikeStun(const uint8_t *data, size_t size)
{
    return size >= 20 && (data[0] & 0xC0) == 0 && be32(data + 4) == kMagicCookie;
}

bool parseResponse(const uint8_t *data, size_t size, Address &mapped, uint8_t (&transaction)[12])
{
    if (!looksLikeStun(data, size) || be16(data) != 0x0101)
        return false;
    const size_t len = be16(data + 2);
    if (20 + len > size)
        return false;
    std::memcpy(transaction, data + 8, 12);
    bool have = false;
    for (size_t i = 20; i + 4 <= 20 + len;)
    {
        const uint16_t type = be16(data + i), alen = be16(data + i + 2);
        const uint8_t *v = data + i + 4;
        if (i + 4 + alen > 20 + len)
            break;
        if ((type == 0x0020 || type == 0x8020) && alen >= 8 && v[1] == 0x01)
        {
            mapped.port = static_cast<uint16_t>(be16(v + 2) ^ (kMagicCookie >> 16));
            mapped.ip = be32(v + 4) ^ kMagicCookie;
            return true; // XOR-MAPPED-ADDRESS wins
        }
        if (type == 0x0001 && alen >= 8 && v[1] == 0x01 && !have)
        {
            mapped.port = be16(v + 2);
            mapped.ip = be32(v + 4);
            have = true;
        }
        i += 4 + ((alen + 3u) & ~3u);
    }
    return have;
}

const std::vector<std::string> &servers()
{
    static const std::vector<std::string> list = {"stun.l.google.com:19302", "stun.cloudflare.com:3478",
                                                  "stun1.l.google.com:19302"};
    return list;
}
} // namespace stun

// ---- PortMapper ----

std::atomic<int> g_mappersRunning{0};

struct PortMapper::Shared
{
    std::mutex mutex;
    Status status;
    std::atomic<bool> stop{false};
    void set(const Status &s)
    {
        std::lock_guard<std::mutex> lock(mutex);
        status = s;
    }
};

namespace
{
void runMapper(std::shared_ptr<PortMapper::Shared> sh, uint16_t port);
}

PortMapper::~PortMapper() { stop(); }

void PortMapper::waitForAll(int ms)
{
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (g_mappersRunning.load() > 0 && Clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

PortMapper::Status PortMapper::status() const
{
    if (!m_shared)
        return {};
    std::lock_guard<std::mutex> lock(m_shared->mutex);
    return m_shared->status;
}

void PortMapper::start(uint16_t port)
{
    stop();
    m_shared = std::make_shared<Shared>();
    m_shared->set(Status{State::Working, "", {}, ""});
    ++g_mappersRunning;
    m_thread = std::thread(runMapper, m_shared, port);
}

void PortMapper::stop()
{
    // The thread removes the mapping and ends on its own: joining could keep
    // the game waiting on a slow router for seconds.
    if (m_shared)
        m_shared->stop = true;
    if (m_thread.joinable())
        m_thread.detach();
}

namespace
{
void runMapperBody(std::shared_ptr<PortMapper::Shared> sh, uint16_t port);
void runMapper(std::shared_ptr<PortMapper::Shared> sh, uint16_t port)
{
    runMapperBody(std::move(sh), port);
    --g_mappersRunning;
}
void runMapperBody(std::shared_ptr<PortMapper::Shared> sh, uint16_t port)
{
    using State = PortMapper::State;
    using Status = PortMapper::Status;
    auto alive = [&] { return !sh->stop.load(); };
    const uint32_t gateway = defaultGateway();
    const uint32_t local = gateway ? localAddressToward(gateway) : 0;
    std::string pmpErr, upnpErr;
    constexpr uint32_t kLifetime = 7200;
    enum class Via
    {
        None,
        Pmp,
        Upnp
    } via = Via::None;
    Igd igd;
    uint16_t external = 0;
    uint32_t publicIp = 0;
    if (gateway)
    {
        external = natpmpMap(gateway, port, kLifetime, pmpErr);
        if (external)
        {
            via = Via::Pmp;
            natpmpExternal(gateway, publicIp);
        }
    }
    else
        pmpErr = "no gateway found";
    if (via == Via::None && alive())
    {
        if (findIgd(igd, upnpErr) && local && upnpMap(igd, port, local, kLifetime, upnpErr))
        {
            via = Via::Upnp;
            external = port;
            std::string e;
            upnpExternal(igd, publicIp, e);
        }
    }
    auto unmap = [&] {
        std::string e;
        if (via == Via::Pmp)
            natpmpMap(gateway, port, 0, e);
        else if (via == Via::Upnp)
            upnpUnmap(igd, port);
        if (via != Via::None)
        {
            std::printf("[nat] asked the router to close port %u again\n", unsigned(port));
            std::fflush(stdout);
        }
    };
    if (!alive())
    {
        unmap();
        return;
    }
    if (via == Via::None)
    {
        sh->set(Status{State::Failed, "", {}, "UPnP: " + upnpErr + "; NAT-PMP: " + pmpErr});
        return;
    }
    Status s{State::Mapped, via == Via::Pmp ? "NAT-PMP" : "UPnP", Address{publicIp, external}, ""};
    if (publicIp && privateIp(publicIp))
        s.detail = "the router's own address (" + ipStr(publicIp) +
                   ") is private: another NAT is in front of it (carrier-grade?), so players outside may not get in";
    sh->set(s);
    // Renew halfway through the lease until told to stop, then remove it.
    auto renewAt = Clock::now() + std::chrono::seconds(kLifetime / 2);
    while (alive())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (Clock::now() < renewAt)
            continue;
        renewAt = Clock::now() + std::chrono::seconds(kLifetime / 2);
        std::string e;
        if (via == Via::Pmp)
            natpmpMap(gateway, port, kLifetime, e);
        else
            upnpMap(igd, port, local, kLifetime, e);
    }
    unmap();
}
} // namespace

std::string probe()
{
    std::string out;
    auto line = [&](const std::string &s) { out += s + "\n"; };
    const uint32_t gateway = defaultGateway();
    const uint32_t local = gateway ? localAddressToward(gateway) : 0;
    line("gateway " + (gateway ? ipStr(gateway) : std::string("unknown")) + ", this machine " +
         (local ? ipStr(local) : std::string("unknown")));
    uint32_t ip = 0;
    line(gateway && natpmpExternal(gateway, ip) ? "NAT-PMP: yes, public address " + ipStr(ip)
                                                : std::string("NAT-PMP: no answer"));
    Igd igd;
    std::string err;
    if (findIgd(igd, err))
    {
        line("UPnP: router at " + ipStr(igd.control.ip) + ":" + std::to_string(igd.control.port) + igd.control.path +
             " (" + igd.service + ")");
        line(upnpExternal(igd, ip, err) ? "UPnP: public address " + ipStr(ip) +
                                              (privateIp(ip) ? " (private: another NAT is in front)" : "")
                                        : "UPnP: " + err);
    }
    else
        line("UPnP: " + err);
    // STUN from one socket to two servers: the same public port both times
    // means hole punching can work.
    UdpSocket s;
    if (!s.open(0))
    {
        line("STUN: no socket");
        return out;
    }
    std::vector<Address> seen;
    for (const std::string &name : stun::servers())
    {
        Address server;
        if (!Address::parse(name, 3478, server))
        {
            line("STUN: cannot find " + name);
            continue;
        }
        uint8_t tx[12];
        for (int i = 0; i < 12; ++i)
            tx[i] = static_cast<uint8_t>(std::rand());
        const std::vector<uint8_t> req = stun::bindingRequest(tx);
        bool got = false;
        for (int attempt = 0; attempt < 3 && !got; ++attempt)
        {
            s.send(server, req.data(), req.size());
            const auto until = Clock::now() + std::chrono::milliseconds(500);
            while (Clock::now() < until && !got)
            {
                uint8_t buf[512];
                Address from;
                const int n = s.receive(from, buf, sizeof(buf));
                if (n < 0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                Address mapped;
                uint8_t rtx[12];
                if (stun::parseResponse(buf, static_cast<size_t>(n), mapped, rtx) && std::memcmp(rtx, tx, 12) == 0)
                {
                    line("STUN " + name + ": the internet sees " + mapped.str() + " (local port " +
                         std::to_string(s.localPort()) + ")");
                    seen.push_back(mapped);
                    got = true;
                }
            }
        }
        if (!got)
            line("STUN " + name + ": no answer");
    }
    if (seen.size() >= 2)
        line(std::all_of(seen.begin(), seen.end(), [&](const Address &a) { return a == seen[0]; })
                 ? "NAT: the same public port for every destination (hole punching should work)"
                 : "NAT: a different public port per destination (symmetric: hole punching will not work)");
    return out;
}
} // namespace kh2net
