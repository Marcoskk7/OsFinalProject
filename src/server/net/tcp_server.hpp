#pragma once

#include "common/logger.hpp"
#include "common/protocol.hpp"
#include "common/thread_pool.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>

namespace osp::net
{

// 多线程 TCP 服务器：使用线程池处理多客户端并发连接
class TcpServer
{
public:
    using RequestHandler = std::function<osp::protocol::Message(const osp::protocol::Message&)>;

    explicit TcpServer(std::uint16_t port, std::size_t poolSize = 4) noexcept;
    ~TcpServer();

    // 禁止拷贝
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // 启动服务器，持续监听并处理客户端连接
    // handler 会在线程池中的工作线程上被调用
    void start(const RequestHandler& handler);

    // 停止服务器
    void stop();

    // 获取线程池大小
    [[nodiscard]] std::size_t poolSize() const noexcept { return poolSize_; }

    // 旧接口（保持兼容，但内部会转发到新实现）
    bool serveOnce(const RequestHandler& handler);

private:
    // 处理单个客户端连接
    void handleClient(int clientFd, const RequestHandler& handler);

    // 内部辅助：发送/接收完整缓冲区
    static bool sendAll(int fd, const void* buf, std::size_t len);
    static bool recvAll(int fd, void* buf, std::size_t len);

    static bool sendMessage(int fd, const osp::protocol::Message& msg);
    static std::optional<osp::protocol::Message> recvMessage(int fd);

    std::uint16_t     port_{};
    std::size_t       poolSize_{};
    std::atomic<bool> running_{false};
    int               listenFd_{-1};
};

} // namespace osp::net
