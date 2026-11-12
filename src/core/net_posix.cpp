// SurfaceRT-WiDi - POSIX socket layer (also used by the Linux dev harness).
// SPDX-License-Identifier: MIT
#if !defined(_WIN32)
#include "widi/net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "widi/log.h"

namespace widi {
namespace {

bool set_nonblocking(int fd, bool on) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    const int want = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, want) == 0;
}

bool wait_readable(int fd, int timeoutMs) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    const int rc = select(fd + 1, &rd, nullptr, nullptr, &tv);
    return rc > 0;
}

class PosixConnection : public IConnection {
public:
    PosixConnection(int fd, const std::string& peer) : fd_(fd), peer_(peer) {}
    ~PosixConnection() override { close(); }

    bool send_all(const void* data, size_t size) override {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        size_t sent = 0;
        while (sent < size) {
            const ssize_t n = ::send(fd_, p + sent, size - sent, MSG_NOSIGNAL);
            if (n > 0) {
                sent += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n >= 0) continue;  // socket buffer full: keep pushing
            return false;
        }
        return true;
    }

    int recv_some(void* buf, size_t cap) override {
        if (fd_ < 0) return -1;
        if (!wait_readable(fd_, 20)) return 0;
        for (;;) {
            const ssize_t n = ::recv(fd_, buf, cap, 0);
            if (n > 0) return static_cast<int>(n);
            if (n == 0) return -1;  // orderly shutdown
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
    }

    void close() override {
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool set_nodelay(bool enable) override {
        const int v = enable ? 1 : 0;
        return setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) == 0;
    }

    std::string peer_address() const override { return peer_; }
    intptr_t native_handle() const override { return fd_; }

private:
    int fd_;
    std::string peer_;
};

bool fill_sockaddr(const std::string& host, uint16_t port, sockaddr_in& out) {
    memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1) return true;

    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    ::freeaddrinfo(res);
    return true;
}

}  // namespace

bool net_init(std::string*) { return true; }
void net_shutdown() {}

TcpListener::~TcpListener() { close(); }

bool TcpListener::listen(uint16_t port, std::string* err) {
    close();
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err) *err = "socket() failed";
        return false;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (err) *err = std::string("bind() failed: ") + strerror(errno);
        ::close(fd);
        return false;
    }
    if (::listen(fd, 4) != 0) {
        if (err) *err = std::string("listen() failed: ") + strerror(errno);
        ::close(fd);
        return false;
    }
    sockaddr_in bound;
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
        port_ = ntohs(bound.sin_port);
    } else {
        port_ = port;
    }
    fd_ = fd;
    return true;
}

std::unique_ptr<IConnection> TcpListener::accept(int timeoutMs, std::string* err) {
    if (fd_ < 0) {
        if (err) *err = "listener closed";
        return nullptr;
    }
    if (!wait_readable(static_cast<int>(fd_), timeoutMs)) return nullptr;
    sockaddr_in peer;
    socklen_t len = sizeof(peer);
    const int fd = ::accept(fd_, reinterpret_cast<sockaddr*>(&peer), &len);
    if (fd < 0) {
        if (err && errno != EAGAIN && errno != EINTR) {
            *err = std::string("accept() failed: ") + strerror(errno);
        }
        return nullptr;
    }
    char ip[64] = {0};
    ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    char label[96];
    snprintf(label, sizeof(label), "%s:%u", ip, ntohs(peer.sin_port));
    return std::unique_ptr<IConnection>(new PosixConnection(fd, label));
}

void TcpListener::close() {
    if (fd_ >= 0) {
        ::close(static_cast<int>(fd_));
        fd_ = -1;
    }
}

std::unique_ptr<IConnection> connect_tcp(const std::string& host, uint16_t port,
                                        int timeoutMs, std::string* err) {
    sockaddr_in addr;
    if (!fill_sockaddr(host, port, addr)) {
        if (err) *err = "cannot resolve host";
        return nullptr;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err) *err = "socket() failed";
        return nullptr;
    }
    set_nonblocking(fd, true);
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0) {
        if (errno != EINPROGRESS) {
            if (err) *err = std::string("connect failed: ") + strerror(errno);
            ::close(fd);
            return nullptr;
        }
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(fd, &wr);
        timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        if (select(fd + 1, nullptr, &wr, nullptr, &tv) <= 0) {
            if (err) *err = "connect timed out";
            ::close(fd);
            return nullptr;
        }
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error != 0) {
            if (err) *err = std::string("connect failed: ") + strerror(so_error);
            ::close(fd);
            return nullptr;
        }
    }
    set_nonblocking(fd, false);
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    char label[128];
    snprintf(label, sizeof(label), "%s:%u", host.c_str(), port);
    return std::unique_ptr<IConnection>(new PosixConnection(fd, label));
}

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::open(uint16_t bindPort, bool reuse, std::string* err) {
    close();
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        if (err) *err = "socket() failed";
        return false;
    }
    if (reuse) {
        const int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(bindPort);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (err) *err = std::string("bind() failed: ") + strerror(errno);
        ::close(fd);
        return false;
    }
    fd_ = fd;
    return true;
}

bool UdpSocket::enable_broadcast(bool on, std::string* err) {
    const int v = on ? 1 : 0;
    if (setsockopt(static_cast<int>(fd_), SOL_SOCKET, SO_BROADCAST, &v, sizeof(v)) != 0) {
        if (err) *err = "SO_BROADCAST failed";
        return false;
    }
    return true;
}

int UdpSocket::send_to(const std::string& ip, uint16_t port, const void* data, size_t size) {
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) return -1;
    const ssize_t n = ::sendto(static_cast<int>(fd_), data, size, 0,
                               reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    return static_cast<int>(n);
}

int UdpSocket::recv_from(void* buf, size_t cap, std::string* fromIp, uint16_t* fromPort,
                         int timeoutMs) {
    if (fd_ < 0) return -1;
    if (!wait_readable(static_cast<int>(fd_), timeoutMs)) return 0;
    sockaddr_in peer;
    socklen_t len = sizeof(peer);
    const ssize_t n = ::recvfrom(static_cast<int>(fd_), buf, cap, 0,
                                 reinterpret_cast<sockaddr*>(&peer), &len);
    if (n < 0) return -1;
    if (fromIp) {
        char ip[64] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        *fromIp = ip;
    }
    if (fromPort) *fromPort = ntohs(peer.sin_port);
    return static_cast<int>(n);
}

void UdpSocket::close() {
    if (fd_ >= 0) {
        ::close(static_cast<int>(fd_));
        fd_ = -1;
    }
}

bool local_ipv4_addresses(std::vector<std::string>& out) {
    out.clear();
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return false;
    for (ifaddrs* it = list; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        char ip[64] = {0};
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(it->ifa_addr)->sin_addr, ip,
                  sizeof(ip));
        out.push_back(ip);
    }
    freeifaddrs(list);
    return true;
}

}  // namespace widi

#endif  // !_WIN32
