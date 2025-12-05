#pragma once

#include "common/logger.hpp"
#include "common/protocol.hpp"

#include <cstdint>
#include <functional>
#include <optional>

namespace osp::net
{

// 简单的阻塞式 TCP 服务器：一次接受一个客户端、收一条消息、回一条消息。
// 仅在 Linux/WSL 下使用 POSIX socket，足够完成课程项目的基本需求。
class TcpServer
{
public:
    explicit TcpServer(std::uint16_t port) noexcept;

    // 启动监听并处理一次请求：接收一条 Message，调用 handler 生成响应，再发送回去。
    // 返回是否成功完成一次往返。
    bool serveOnce(const std::function<osp::protocol::Message(const osp::protocol::Message&)>& handler);

private:
    std::uint16_t port_{};

    // 内部辅助：发送/接收完整缓冲区
    static bool sendAll(int fd, const void* buf, std::size_t len);
    static bool recvAll(int fd, void* buf, std::size_t len);

    static bool sendMessage(int fd, const osp::protocol::Message& msg);
    static std::optional<osp::protocol::Message> recvMessage(int fd);
};

} // namespace osp::net



