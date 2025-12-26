#pragma once

#include "common/protocol.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace osp::net
{

// 简单的阻塞式 TCP 客户端：连接服务器、发送一条 Message、接收一条响应。
class TcpClient
{
public:
    TcpClient(std::string host, std::uint16_t port);

    // 连接到服务器，发送 req，并等待响应；失败时返回 std::nullopt。
    std::optional<osp::protocol::Message> request(const osp::protocol::Message& req);

private:
    std::string host_;
    std::uint16_t port_{};

    static bool sendAll(int fd, const void* buf, std::size_t len);
    static bool recvAll(int fd, void* buf, std::size_t len);

    static bool sendMessage(int fd, const osp::protocol::Message& msg);
    static std::optional<osp::protocol::Message> recvMessage(int fd);
};

} // namespace osp::net



