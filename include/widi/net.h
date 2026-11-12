// SurfaceRT-WiDi - minimal socket layer (POSIX + Winsock implementations).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace widi {

bool net_init(std::string* err);
void net_shutdown();

class IConnection {
public:
    virtual ~IConnection() {}
    // Sends everything or fails.
    virtual bool send_all(const void* data, size_t size) = 0;
    // > 0 : bytes read; 0 : nothing available yet; - 1 : closed or broken.
    virtual int recv_some(void* buf, size_t cap) = 0;
    virtual void close() = 0;
    virtual bool set_nodelay(bool enable) = 0;
    virtual std::string peer_address() const = 0;
    virtual intptr_t native_handle() const = 0;
};

class TcpListener {
public:
    TcpListener() {}
    ~TcpListener();
    bool listen(uint16_t port, std::string* err);
    std::unique_ptr<IConnection> accept(int timeoutMs, std::string* err);
    void close();
    uint16_t port() const { return port_; }
    bool valid() const { return fd_ >= 0; }

private:
    TcpListener(const TcpListener&);
    TcpListener& operator=(const TcpListener&);

    intptr_t fd_ = -1;
    uint16_t port_ = 0;
};

std::unique_ptr<IConnection> connect_tcp(const std::string& host, uint16_t port,
                                         int timeoutMs, std::string* err);

class UdpSocket {
public:
    UdpSocket() {}
    ~UdpSocket();
    bool open(uint16_t bindPort, bool reuse, std::string* err);
    bool enable_broadcast(bool on, std::string* err);
    int send_to(const std::string& ip, uint16_t port, const void* data, size_t size);
    // > 0 : bytes; 0 : timeout; - 1 : error.
    int recv_from(void* buf, size_t cap, std::string* fromIp, uint16_t* fromPort,
                  int timeoutMs);
    void close();
    bool valid() const { return fd_ >= 0; }

private:
    UdpSocket(const UdpSocket&);
    UdpSocket& operator=(const UdpSocket&);

    intptr_t fd_ = -1;
};

// Fills `out` with the IPv4 addresses of the local interfaces.
bool local_ipv4_addresses(std::vector<std::string>& out);

}  // namespace widi
