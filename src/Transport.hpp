#pragma once
#include <string>
#include <memory>
#include <deque>
#include "Config.hpp"

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual bool        open()                       = 0;
    virtual void        close()                      = 0;
    virtual bool        isOpen() const               = 0;
    // Returns one complete NMEA line (no \r\n), or "" on timeout/error.
    virtual std::string readLine(int timeout_ms = 100) = 0;
    // Writes data to the transport. Returns false on error or if not supported.
    virtual bool send(const std::string& data) = 0;
};

// UDP socket bound to a local port — recvfrom
class UdpServerTransport : public ITransport {
public:
    explicit UdpServerTransport(const TransportDef& cfg);
    ~UdpServerTransport() override;

    bool        open()  override;
    void        close() override;
    bool        isOpen() const override { return fd_ >= 0; }
    std::string readLine(int timeout_ms = 100) override;
    bool        send(const std::string& data) override;

private:
    TransportDef     cfg_;
    int              fd_ = -1;
    std::deque<std::string> buf_;

    void drainDatagram();
};

// TCP socket connecting to a remote server with auto-reconnect
class TcpClientTransport : public ITransport {
public:
    explicit TcpClientTransport(const TransportDef& cfg);
    ~TcpClientTransport() override;

    bool        open()  override;
    void        close() override;
    bool        isOpen() const override { return fd_ >= 0; }
    std::string readLine(int timeout_ms = 100) override;
    bool        send(const std::string& data) override;

private:
    TransportDef    cfg_;
    int             fd_ = -1;
    std::string     recv_buf_;

    bool connect();
    void disconnect();
};

// TCP server: binds, accepts one client, reads from it
class TcpServerTransport : public ITransport {
public:
    explicit TcpServerTransport(const TransportDef& cfg);
    ~TcpServerTransport() override;

    bool        open()  override;
    void        close() override;
    bool        isOpen() const override { return client_fd_ >= 0; }
    std::string readLine(int timeout_ms = 100) override;
    bool        send(const std::string& data) override;

private:
    TransportDef    cfg_;
    int             listen_fd_  = -1;
    int             client_fd_  = -1;
    std::string     recv_buf_;

    bool acceptClient(int timeout_ms);
};

std::unique_ptr<ITransport> makeTransport(const TransportDef& cfg);
