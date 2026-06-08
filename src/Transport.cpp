#include "Transport.hpp"
#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <chrono>
#include <thread>

// POSIX headers
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

// ── helpers ───────────────────────────────────────────────────────────────────

static std::string strerr() { return std::string(std::strerror(errno)); }

static void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static bool pollReadable(int fd, int timeout_ms) {
    struct pollfd pfd = { fd, POLLIN, 0 };
    return poll(&pfd, 1, timeout_ms) > 0;
}

static void splitIntoLines(const char* buf, ssize_t n, std::deque<std::string>& out) {
    std::string data(buf, n);
    size_t pos = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) {
            // partial line — keep in a remainder (caller handles)
            out.push_back(data.substr(pos));
            break;
        }
        std::string line = data.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(std::move(line));
        pos = nl + 1;
    }
}

// ── UdpServerTransport ────────────────────────────────────────────────────────

UdpServerTransport::UdpServerTransport(const TransportDef& cfg) : cfg_(cfg) {}

UdpServerTransport::~UdpServerTransport() { close(); }

bool UdpServerTransport::open() {
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;

    int yes = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(cfg_.bind_port));
    inet_pton(AF_INET, cfg_.bind_host.c_str(), &addr.sin_addr);

    if (bind(fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(fd_); fd_ = -1;
        return false;
    }
    setNonBlocking(fd_);
    return true;
}

void UdpServerTransport::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    buf_.clear();
}

void UdpServerTransport::drainDatagram() {
    char raw[4096];
    ssize_t n = recvfrom(fd_, raw, sizeof(raw)-1, 0, nullptr, nullptr);
    if (n <= 0) return;
    splitIntoLines(raw, n, buf_);
}

std::string UdpServerTransport::readLine(int timeout_ms) {
    if (!buf_.empty()) {
        auto line = buf_.front(); buf_.pop_front(); return line;
    }
    if (!pollReadable(fd_, timeout_ms)) return "";
    drainDatagram();
    if (buf_.empty()) return "";
    auto line = buf_.front(); buf_.pop_front(); return line;
}

bool UdpServerTransport::send(const std::string&) { return false; }

// ── TcpClientTransport ────────────────────────────────────────────────────────

TcpClientTransport::TcpClientTransport(const TransportDef& cfg) : cfg_(cfg) {}

TcpClientTransport::~TcpClientTransport() { close(); }

bool TcpClientTransport::connect() {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    struct addrinfo hints{}, *res;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(cfg_.port);
    if (getaddrinfo(cfg_.host.c_str(), port_str.c_str(), &hints, &res) != 0) {
        ::close(fd_); fd_ = -1; return false;
    }

    // Non-blocking connect with timeout
    setNonBlocking(fd_);
    int rc = ::connect(fd_, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        ::close(fd_); fd_ = -1; return false;
    }

    // Wait for connection
    struct pollfd pfd = { fd_, POLLOUT, 0 };
    if (poll(&pfd, 1, cfg_.connect_timeout_sec * 1000) <= 0) {
        ::close(fd_); fd_ = -1; return false;
    }
    int err = 0; socklen_t len = sizeof(err);
    getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) { ::close(fd_); fd_ = -1; return false; }
    return true;
}

void TcpClientTransport::disconnect() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    recv_buf_.clear();
}

bool TcpClientTransport::open() { return connect(); }

void TcpClientTransport::close() { disconnect(); }

std::string TcpClientTransport::readLine(int timeout_ms) {
    // Return buffered line if available
    auto nl = recv_buf_.find('\n');
    if (nl != std::string::npos) {
        std::string line = recv_buf_.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        recv_buf_.erase(0, nl + 1);
        return line;
    }

    if (fd_ < 0) {
        // Try reconnect
        std::this_thread::sleep_for(std::chrono::seconds(cfg_.reconnect_delay_sec));
        connect();
        return "";
    }

    if (!pollReadable(fd_, timeout_ms)) return "";

    char buf[4096];
    ssize_t n = recv(fd_, buf, sizeof(buf)-1, 0);
    if (n <= 0) {
        // Connection closed/error — schedule reconnect
        disconnect();
        return "";
    }
    recv_buf_.append(buf, n);

    nl = recv_buf_.find('\n');
    if (nl == std::string::npos) return "";
    std::string line = recv_buf_.substr(0, nl);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    recv_buf_.erase(0, nl + 1);
    return line;
}

bool TcpClientTransport::send(const std::string& data) {
    if (fd_ < 0) return false;
    ssize_t n = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
    return n == static_cast<ssize_t>(data.size());
}

// ── TcpServerTransport ────────────────────────────────────────────────────────

TcpServerTransport::TcpServerTransport(const TransportDef& cfg) : cfg_(cfg) {}

TcpServerTransport::~TcpServerTransport() { close(); }

bool TcpServerTransport::open() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;

    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(cfg_.bind_port));
    inet_pton(AF_INET, cfg_.bind_host.c_str(), &addr.sin_addr);

    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0 ||
        listen(listen_fd_, 1) < 0) {
        ::close(listen_fd_); listen_fd_ = -1; return false;
    }
    setNonBlocking(listen_fd_);
    return acceptClient(cfg_.connect_timeout_sec * 1000);
}

void TcpServerTransport::close() {
    if (client_fd_ >= 0) { ::close(client_fd_); client_fd_ = -1; }
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    recv_buf_.clear();
}

bool TcpServerTransport::acceptClient(int timeout_ms) {
    if (!pollReadable(listen_fd_, timeout_ms)) return false;
    sockaddr_in caddr{}; socklen_t clen = sizeof(caddr);
    client_fd_ = accept(listen_fd_, (sockaddr*)&caddr, &clen);
    if (client_fd_ < 0) return false;
    setNonBlocking(client_fd_);
    return true;
}

std::string TcpServerTransport::readLine(int timeout_ms) {
    // Re-accept if client disconnected
    if (client_fd_ < 0) {
        recv_buf_.clear();
        if (!acceptClient(timeout_ms)) return "";
    }

    auto nl = recv_buf_.find('\n');
    if (nl != std::string::npos) {
        std::string line = recv_buf_.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        recv_buf_.erase(0, nl + 1);
        return line;
    }

    if (!pollReadable(client_fd_, timeout_ms)) return "";

    char buf[4096];
    ssize_t n = recv(client_fd_, buf, sizeof(buf)-1, 0);
    if (n <= 0) {
        ::close(client_fd_); client_fd_ = -1;
        return "";
    }
    recv_buf_.append(buf, n);

    nl = recv_buf_.find('\n');
    if (nl == std::string::npos) return "";
    std::string line = recv_buf_.substr(0, nl);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    recv_buf_.erase(0, nl + 1);
    return line;
}

bool TcpServerTransport::send(const std::string& data) {
    if (client_fd_ < 0) return false;
    ssize_t n = ::send(client_fd_, data.data(), data.size(), MSG_NOSIGNAL);
    if (n != static_cast<ssize_t>(data.size())) {
        ::close(client_fd_); client_fd_ = -1;
        return false;
    }
    return true;
}

// ── factory ───────────────────────────────────────────────────────────────────

std::unique_ptr<ITransport> makeTransport(const TransportDef& cfg) {
    if (cfg.type == "udp_server")
        return std::make_unique<UdpServerTransport>(cfg);
    if (cfg.type == "tcp_client")
        return std::make_unique<TcpClientTransport>(cfg);
    if (cfg.type == "tcp_server")
        return std::make_unique<TcpServerTransport>(cfg);
    throw std::runtime_error("Unknown transport type: " + cfg.type);
}
