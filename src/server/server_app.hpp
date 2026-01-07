#pragma once

#include "common/logger.hpp"
#include "common/protocol.hpp"

#include "filesystem/vfs.hpp"
#include "server/net/tcp_server.hpp"

#include "domain/auth.hpp"

#include "server/handlers/admin_handler.hpp"
#include "server/handlers/author_handler.hpp"
#include "server/handlers/editor_handler.hpp"
#include "server/handlers/reviewer_handler.hpp"
#include "server/services/fs_service.hpp"
#include "server/services/paper_service.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace osp::server
{

// 负责加载文件系统、处理客户端请求的服务器核心类
// 支持多线程并发处理客户端请求
class ServerApp
{
public:
    explicit ServerApp(std::uint16_t port,
                       std::size_t   cacheCapacity = 64,
                       std::size_t   threadPoolSize = 4);

    void run();    // 启动服务器（阻塞）
    void stop();   // 停止服务器

    // 获取配置信息
    [[nodiscard]] std::size_t threadPoolSize() const noexcept { return threadPoolSize_; }

private:
    osp::protocol::Message handleRequest(const osp::protocol::Message& req);

    // 统一命令路由入口：根据命令名分发到不同子处理函数
    osp::protocol::Message
    handleCommand(const osp::protocol::Command&                        cmd,
                  const std::optional<osp::domain::Session>& maybeSession);

    // 初始化 AuthService 的 VFS 操作接口
    void initAuthVfsOperations();

    std::uint16_t            port_{};
    std::size_t              threadPoolSize_{};
    std::atomic<bool>        running_{false};
    osp::fs::Vfs             vfs_;
    osp::domain::AuthService auth_; // 认证与会话管理

    // 互斥锁保护共享资源（必须先于 service/handler 构造）
    mutable std::mutex vfsMutex_;   // 保护 VFS 访问
    mutable std::mutex authMutex_;  // 保护 AuthService 访问

    // 服务与处理器（降低 ServerApp 耦合：ServerApp 只做“路由 + 资源持有”）
    FsService    fsService_;
    PaperService paperService_;

    AdminHandler    adminHandler_;
    EditorHandler   editorHandler_;
    AuthorHandler   authorHandler_;
    ReviewerHandler reviewerHandler_;
};

} // namespace osp::server
