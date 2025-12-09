#pragma once

#include "common/logger.hpp"
#include "common/protocol.hpp"

#include "filesystem/vfs.hpp"
#include "server/net/tcp_server.hpp"

#include "domain/auth.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace osp::server
{

// 负责加载文件系统、处理客户端请求的服务器核心类
class ServerApp
{
public:
    explicit ServerApp(std::uint16_t port);

    void run();    // 后续可改为事件循环
    void stop();   // 停止服务器

private:
    osp::protocol::Message handleRequest(const osp::protocol::Message& req);

    // 统一命令路由入口：根据命令名分发到不同子处理函数
    osp::protocol::Message
    handleCommand(const osp::protocol::Command&                        cmd,
                  const std::optional<osp::domain::Session>& maybeSession);

    // 目前已实现的一小部分命令分组：文件系统相关命令
    osp::protocol::Message
    handleFsCommand(const osp::protocol::Command&                        cmd,
                    const std::optional<osp::domain::Session>& maybeSession);

    std::uint16_t          port_{};
    std::atomic<bool>      running_{false};
    fs::Vfs                vfs_;
    osp::domain::AuthService auth_; // 认证与会话管理
};

} // namespace osp::server


