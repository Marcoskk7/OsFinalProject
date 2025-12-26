#include "server/net/tcp_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <string>

namespace osp::net
{

TcpServer::TcpServer(std::uint16_t port, std::size_t poolSize) noexcept
    : port_(port)
    , poolSize_(poolSize)
{
}

TcpServer::~TcpServer()
{
    stop();
}

bool TcpServer::sendAll(int fd, const void* buf, std::size_t len)
{
    const auto* p = static_cast<const std::uint8_t*>(buf);
    std::size_t sent = 0;
    while (sent < len)
    {
        const auto n = ::send(fd, p + sent, len - sent, 0);
        if (n <= 0)
        {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool TcpServer::recvAll(int fd, void* buf, std::size_t len)
{
    auto* p = static_cast<std::uint8_t*>(buf);
    std::size_t recvd = 0;
    while (recvd < len)
    {
        const auto n = ::recv(fd, p + recvd, len - recvd, 0);
        if (n <= 0)
        {
            return false;
        }
        recvd += static_cast<std::size_t>(n);
    }
    return true;
}

bool TcpServer::sendMessage(int fd, const osp::protocol::Message& msg)
{
    const std::string data = osp::protocol::serialize(msg);
    std::uint32_t len = static_cast<std::uint32_t>(data.size());
    len = htonl(len);
    if (!sendAll(fd, &len, sizeof(len)))
    {
        return false;
    }
    return sendAll(fd, data.data(), data.size());
}

std::optional<osp::protocol::Message> TcpServer::recvMessage(int fd)
{
    std::uint32_t len = 0;
    if (!recvAll(fd, &len, sizeof(len)))
    {
        return std::nullopt;
    }
    len = ntohl(len);
    if (len == 0)
    {
        return std::nullopt;
    }

    std::string data;
    data.resize(len);
    if (!recvAll(fd, data.data(), data.size()))
    {
        return std::nullopt;
    }

    return osp::protocol::deserialize(data);
}

void TcpServer::handleClient(int clientFd, const RequestHandler& handler)
{
    osp::log(osp::LogLevel::Info, "TcpServer: handling client on fd " + std::to_string(clientFd));

    // 循环处理该客户端的多个请求（支持持久连接）
    while (running_.load())
    {
        auto maybeReq = recvMessage(clientFd);
        if (!maybeReq)
        {
            osp::log(osp::LogLevel::Info, "TcpServer: client disconnected (fd " + std::to_string(clientFd) + ")");
            break;
        }

        const auto& req = *maybeReq;
        osp::log(osp::LogLevel::Debug, "TcpServer: received request from fd " + std::to_string(clientFd));

        const auto resp = handler(req);
        if (!sendMessage(clientFd, resp))
        {
            osp::log(osp::LogLevel::Warn, "TcpServer: failed to send response to fd " + std::to_string(clientFd));
            break;
        }
    }

    ::close(clientFd);
    osp::log(osp::LogLevel::Debug, "TcpServer: closed client fd " + std::to_string(clientFd));
}

void TcpServer::start(const RequestHandler& handler)
{
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0)
    {
        osp::log(osp::LogLevel::Error, "TcpServer: failed to create socket");
        return;
    }

    int opt = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        osp::log(osp::LogLevel::Error, "TcpServer: bind failed");
        ::close(listenFd_);
        listenFd_ = -1;
        return;
    }

    // 增加 backlog 以支持更多并发连接
    if (::listen(listenFd_, 128) < 0)
    {
        osp::log(osp::LogLevel::Error, "TcpServer: listen failed");
        ::close(listenFd_);
        listenFd_ = -1;
        return;
    }

    running_.store(true);
    osp::log(osp::LogLevel::Info,
             "TcpServer: listening on port " + std::to_string(port_)
                 + " with thread pool size " + std::to_string(poolSize_));

    // 创建线程池
    osp::ThreadPool pool(poolSize_);

    while (running_.load())
    {
        sockaddr_in clientAddr{};
        socklen_t   clientLen = sizeof(clientAddr);

        const int clientFd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientFd < 0)
        {
            if (running_.load())
            {
                osp::log(osp::LogLevel::Warn, "TcpServer: accept failed");
            }
            continue;
        }

        // 获取客户端地址信息
        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, INET_ADDRSTRLEN);
        osp::log(osp::LogLevel::Info,
                 "TcpServer: accepted connection from " + std::string(clientIp)
                     + ":" + std::to_string(ntohs(clientAddr.sin_port)));

        // 提交到线程池处理
        pool.enqueue([this, clientFd, &handler]() {
            handleClient(clientFd, handler);
        });
    }

    osp::log(osp::LogLevel::Info, "TcpServer: stopped accepting connections");
}

void TcpServer::stop()
{
    running_.store(false);

    // 关闭监听 socket 以中断 accept
    if (listenFd_ >= 0)
    {
        ::close(listenFd_);
        listenFd_ = -1;
    }
}

// 旧接口：保持向后兼容
bool TcpServer::serveOnce(const RequestHandler& handler)
{
    const int sockFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sockFd < 0)
    {
        osp::log(osp::LogLevel::Error, "TcpServer: failed to create socket");
        return false;
    }

    int opt = 1;
    ::setsockopt(sockFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    if (::bind(sockFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        osp::log(osp::LogLevel::Error, "TcpServer: bind failed");
        ::close(sockFd);
        return false;
    }

    if (::listen(sockFd, 1) < 0)
    {
        osp::log(osp::LogLevel::Error, "TcpServer: listen failed");
        ::close(sockFd);
        return false;
    }

    osp::log(osp::LogLevel::Info, "TcpServer: waiting for connection...");

    const int clientFd = ::accept(sockFd, nullptr, nullptr);
    if (clientFd < 0)
    {
        osp::log(osp::LogLevel::Error, "TcpServer: accept failed");
        ::close(sockFd);
        return false;
    }

    auto maybeReq = recvMessage(clientFd);
    if (!maybeReq)
    {
        osp::log(osp::LogLevel::Error, "TcpServer: failed to receive message");
        ::close(clientFd);
        ::close(sockFd);
        return false;
    }

    const auto& req = *maybeReq;
    osp::log(osp::LogLevel::Info, "TcpServer: received request");

    const auto resp = handler(req);
    const bool ok = sendMessage(clientFd, resp);

    ::close(clientFd);
    ::close(sockFd);

    return ok;
}

} // namespace osp::net
