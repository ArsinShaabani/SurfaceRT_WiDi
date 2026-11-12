// SurfaceRT-WiDi - Winsock implementation of the socket layer (Windows RT 8.1 / ARM).
// SPDX-License-Identifier: MIT
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <stdio.h>
#include <string>

#include "widi/log.h"
#include "widi/net.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace widi {
namespace {

bool wait_readable(SOCKET s, int timeoutMs) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    const int rc = ::select(0, &rd, nullptr, nullptr, &tv);
    return rc > 0;
}

std::string describe_peer(const sockaddr_in& addr) {
    char ip[64] = {0};
    ::InetNtopA(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    char label[96];
    ::_snprintf_s(label, sizeof(label), _TRUNCATE, "%s:%u", ip,
                  static_cast<unsigned>(::ntohs(addr.sin_port)));
    return std::string(label);
}

class WinConnection : public IConnection {
public:
    WinConnection(SOCKET s, const std::string& peer) : sock_(s), peer_(peer) {}
    ~WinConnection() override { close(); }

    bool send_all(const void* data, size_t size) override {
        const char* p = static_cast<const char*>(data);
        size_t sent = 0;
        while (sent < size) {
            const int n = ::send(sock_, p + sent, static_cast<int>(size - sent), 0);
            if (n > 0) {
                sent += static_cast<size_t>(n);
                continue;
            }
            if (n == SOCKET_ERROR) {
                const int err = ::WSAGetLastError();
                if (err == WSAEWOULDBLOCK) {
                    fd_set wr;
                    FD_ZERO(&wr);
                    FD_SET(sock_, &wr);
                    timeval tv = {1, 0};
                    ::select(0, nullptr, &wr, nullptr, &tv);
                    continue;
                }
                return false;
            }
        }
        return true;
    }

    int recv_some(void* buf, size_t cap) override {
        if (sock_ == INVALID_SOCKET) return -1;
        if (!wait_readable(sock_, 20)) return 0;
        const int n = ::recv(sock_, static_cast<char*>(buf), static_cast<int>(cap), 0);
        if (n > 0) return n;
        if (n == 0) return -1;  // orderly shutdown
        const int err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return 0;
        return -1;
    }

    void close() override {
        if (sock_ != INVALID_SOCKET) {
            ::shutdown(sock_, SD_BOTH);
            ::closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
    }

    bool set_nodelay(bool enable) override {
        BOOL v = enable ? TRUE : FALSE;
        return ::setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY,
                            reinterpret_cast<const char*>(&v), sizeof(v)) == 0;
    }

    std::string peer_address() const override { return peer_; }
    intptr_t native_handle() const override { return static_cast<intptr_t>(sock_); }

private:
    SOCKET sock_;
    std::string peer_;
};

bool resolve_ipv4(const std::string& host, uint16_t port, sockaddr_in& out) {
    ZeroMemory(&out, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = ::htons(port);
    if (::InetPtonA(AF_INET, host.c_str(), &out.sin_addr) == 1) return true;

    addrinfo hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    ::freeaddrinfo(res);
    return true;
}

}  // namespace

bool net_init(std::string* err) {
    static bool started = false;
    if (started) return true;
    WSADATA data;
    const int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
        if (err) *err = "WSAStartup failed";
        return false;
    }
    started = true;
    return true;
}

void net_shutdown() { ::WSACleanup(); }

}  // namespace widi

namespace widi {

TcpListener::~TcpListener() { close(); }

bool TcpListener::listen(uint16_t port, std::string* err) {
    close();
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        if (err) *err = "socket() failed";
        return false;
    }
    BOOL one = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = ::htons(port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (err) *err = "bind() failed (port in use?)";
        ::closesocket(s);
        return false;
    }
    if (::listen(s, 4) != 0) {
        if (err) *err = "listen() failed";
        ::closesocket(s);
        return false;
    }
    sockaddr_in bound;
    int len = sizeof(bound);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
        port_ = ::ntohs(bound.sin_port);
    } else {
        port_ = port;
    }
    fd_ = static_cast<intptr_t>(s);
    return true;
}

std::unique_ptr<IConnection> TcpListener::accept(int timeoutMs, std::string* err) {
    if (fd_ < 0) {
        if (err) *err = "listener closed";
        return nullptr;
    }
    SOCKET s = static_cast<SOCKET>(fd_);
    if (!wait_readable(s, timeoutMs)) return nullptr;
    sockaddr_in peer;
    int len = sizeof(peer);
    SOCKET client = ::accept(s, reinterpret_cast<sockaddr*>(&peer), &len);
    if (client == INVALID_SOCKET) return nullptr;
    BOOL one = TRUE;
    ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                 sizeof(one));
    return std::unique_ptr<IConnection>(new WinConnection(client, describe_peer(peer)));
}

void TcpListener::close() {
    if (fd_ >= 0) {
        ::closesocket(static_cast<SOCKET>(fd_));
        fd_ = -1;
    }
}

std::unique_ptr<IConnection> connect_tcp(const std::string& host, uint16_t port,
                                         int timeoutMs, std::string* err) {
    sockaddr_in addr;
    if (!resolve_ipv4(host, port, addr)) {
        if (err) *err = "cannot resolve host";
        return nullptr;
    }
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        if (err) *err = "socket() failed";
        return nullptr;
    }
    u_long nonBlocking = 1;
    ::ioctlsocket(s, FIONBIO, &nonBlocking);
    int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == SOCKET_ERROR) {
        if (::WSAGetLastError() != WSAEWOULDBLOCK) {
            if (err) *err = "connect failed";
            ::closesocket(s);
            return nullptr;
        }
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(s, &wr);
        timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        if (::select(0, nullptr, &wr, nullptr, &tv) <= 0) {
            if (err) *err = "connect timed out";
            ::closesocket(s);
            return nullptr;
        }
        int soError = 0;
        int soLen = sizeof(soError);
        ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &soLen);
        if (soError != 0) {
            if (err) *err = "connect refused";
            ::closesocket(s);
            return nullptr;
        }
    }
    nonBlocking = 0;
    ::ioctlsocket(s, FIONBIO, &nonBlocking);
    BOOL one = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in self;
    int selfLen = sizeof(self);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&self), &selfLen);
    char ip[64] = {0};
    ::InetNtopA(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    char label[96];
    ::_snprintf_s(label, sizeof(label), _TRUNCATE, "%s:%u", ip,
                  static_cast<unsigned>(port));
    return std::unique_ptr<IConnection>(new WinConnection(s, std::string(label)));
}

}  // namespace widi

namespace widi {

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::open(uint16_t bindPort, bool reuse, std::string* err) {
    close();
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        if (err) *err = "socket() failed";
        return false;
    }
    if (reuse) {
        BOOL one = TRUE;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
                     sizeof(one));
    }
    sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = ::htons(bindPort);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (err) *err = "udp bind() failed";
        ::closesocket(s);
        return false;
    }
    fd_ = static_cast<intptr_t>(s);
    return true;
}

bool UdpSocket::enable_broadcast(bool on, std::string* err) {
    BOOL v = on ? TRUE : FALSE;
    if (::setsockopt(static_cast<SOCKET>(fd_), SOL_SOCKET, SO_BROADCAST,
                     reinterpret_cast<const char*>(&v), sizeof(v)) != 0) {
        if (err) *err = "SO_BROADCAST failed";
        return false;
    }
    return true;
}

int UdpSocket::send_to(const std::string& ip, uint16_t port, const void* data, size_t size) {
    sockaddr_in addr;
    if (!resolve_ipv4(ip, port, addr)) return -1;
    const int n = ::sendto(static_cast<SOCKET>(fd_), static_cast<const char*>(data),
                           static_cast<int>(size), 0,
                           reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return n;
}

int UdpSocket::recv_from(void* buf, size_t cap, std::string* fromIp, uint16_t* fromPort,
                         int timeoutMs) {
    if (fd_ < 0) return -1;
    SOCKET s = static_cast<SOCKET>(fd_);
    if (!wait_readable(s, timeoutMs)) return 0;
    sockaddr_in peer;
    int len = sizeof(peer);
    const int n = ::recvfrom(s, static_cast<char*>(buf), static_cast<int>(cap), 0,
                             reinterpret_cast<sockaddr*>(&peer), &len);
    if (n == SOCKET_ERROR) return -1;
    if (fromIp) {
        char ip[64] = {0};
        ::InetNtopA(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        *fromIp = ip;
    }
    if (fromPort) *fromPort = ::ntohs(peer.sin_port);
    return n;
}

void UdpSocket::close() {
    if (fd_ >= 0) {
        ::closesocket(static_cast<SOCKET>(fd_));
        fd_ = -1;
    }
}

bool local_ipv4_addresses(std::vector<std::string>& out) {
    out.clear();
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    IP_ADAPTER_ADDRESSES* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(&buffer[0]);
    ULONG rc = ::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                                  GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, addrs, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(&buffer[0]);
        rc = ::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                                  GAA_FLAG_SKIP_DNS_SERVER,
                                    nullptr, addrs, &size);
    }
    if (rc != NO_ERROR) return false;
    for (IP_ADAPTER_ADDRESSES* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const sockaddr_in* sin =
                reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            char ip[64] = {0};
            ::InetNtopA(AF_INET, const_cast<IN_ADDR*>(&sin->sin_addr), ip, sizeof(ip));
            out.push_back(ip);
        }
    }
    return true;
}

}  // namespace widi

#endif  // _WIN32
