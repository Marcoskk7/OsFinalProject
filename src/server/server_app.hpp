#pragma once

#include "common/logger.hpp"
#include "common/protocol.hpp"

#include "filesystem/vfs.hpp"
#include "server/net/tcp_server.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace osp::server
{

// 负责加载文件系统、处理客户端请求的服务器核心类（暂时不实现真实网络，只提供占位接口）
class ServerApp
{
public:
    explicit ServerApp(std::uint16_t port);

    void run();    // 后续可改为事件循环
    void stop();   // 停止服务器

private:
    osp::protocol::Message handleRequest(const osp::protocol::Message& req);

    std::uint16_t port_{};
    std::atomic<bool> running_{false};
    fs::Vfs vfs_;
};

} // namespace osp::server


