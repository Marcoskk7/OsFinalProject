#include "client/net/tcp_client.hpp"

#include "common/logger.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace osp::net
{

TcpClient::TcpClient(std::string host, std::uint16_t port)
    : host_(std::move(host))
    , port_(port)
{
}

bool TcpClient::sendAll(int fd, const void* buf, std::size_t len)
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

bool TcpClient::recvAll(int fd, void* buf, std::size_t len)
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

bool TcpClient::sendMessage(int fd, const osp::protocol::Message& msg)
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

std::optional<osp::protocol::Message> TcpClient::recvMessage(int fd)
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

std::optional<osp::protocol::Message> TcpClient::request(const osp::protocol::Message& req)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        osp::log(osp::LogLevel::Error, "TcpClient: failed to create socket");
        return std::nullopt;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0)
    {
        osp::log(osp::LogLevel::Error, "TcpClient: invalid host");
        ::close(fd);
        return std::nullopt;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        osp::log(osp::LogLevel::Error, "TcpClient: connect failed");
        ::close(fd);
        return std::nullopt;
    }

    if (!sendMessage(fd, req))
    {
        osp::log(osp::LogLevel::Error, "TcpClient: failed to send message");
        ::close(fd);
        return std::nullopt;
    }

    auto resp = recvMessage(fd);
    if (!resp)
    {
        osp::log(osp::LogLevel::Error, "TcpClient: failed to receive response");
    }

    ::close(fd);
    return resp;
}

} // namespace osp::net



