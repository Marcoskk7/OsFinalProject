#include "server/net/tcp_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>

namespace osp::net
{

TcpServer::TcpServer(std::uint16_t port) noexcept
    : port_(port)
{
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

bool TcpServer::serveOnce(const std::function<osp::protocol::Message(const osp::protocol::Message&)>& handler)
{
    const int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0)
    {
        osp::log(osp::LogLevel::Error, "TcpServer: failed to create socket");
        return false;
    }

    int opt = 1;
    ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    if (::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        osp::log(osp::LogLevel::Error, "TcpServer: bind failed");
        ::close(listenFd);
        return false;
    }

    if (::listen(listenFd, 1) < 0)
    {
        osp::log(osp::LogLevel::Error, "TcpServer: listen failed");
        ::close(listenFd);
        return false;
    }

    osp::log(osp::LogLevel::Info, "TcpServer: waiting for connection...");

    const int clientFd = ::accept(listenFd, nullptr, nullptr);
    if (clientFd < 0)
    {
        osp::log(osp::LogLevel::Error, "TcpServer: accept failed");
        ::close(listenFd);
        return false;
    }

    auto maybeReq = recvMessage(clientFd);
    if (!maybeReq)
    {
        osp::log(osp::LogLevel::Error, "TcpServer: failed to receive message");
        ::close(clientFd);
        ::close(listenFd);
        return false;
    }

    const auto& req = *maybeReq;
    osp::log(osp::LogLevel::Info, "TcpServer: received request");

    const auto resp = handler(req);
    const bool ok = sendMessage(clientFd, resp);

    ::close(clientFd);
    ::close(listenFd);

    return ok;
}

} // namespace osp::net



