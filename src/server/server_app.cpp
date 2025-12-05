#include "server_app.hpp"

namespace osp::server
{

ServerApp::ServerApp(std::uint16_t port)
    : port_(port)
    , vfs_(64) // 简单设置缓存容量
{
}

void ServerApp::run()
{
    running_.store(true);
    osp::log(osp::LogLevel::Info, "Server starting on port " + std::to_string(port_));

    // 挂载简化 VFS
    vfs_.mount("data.fs");

    // 使用阻塞式 TCP 服务器，处理一次真实客户端请求
    osp::net::TcpServer tcpServer(port_);
    tcpServer.serveOnce([this](const osp::protocol::Message& req) {
        return handleRequest(req);
    });

    osp::log(osp::LogLevel::Info, "Server shutting down");
}

void ServerApp::stop()
{
    running_.store(false);
}

osp::protocol::Message ServerApp::handleRequest(const osp::protocol::Message& req)
{
    using osp::protocol::Message;
    using osp::protocol::MessageType;

    osp::log(osp::LogLevel::Info, "Received request payload: " + req.payload);

    // 未来这里应根据 MessageType 和 payload 调用业务层 & VFS。
    // 当前仅支持一个简单命令：LIST_PAPERS。
    if (req.type == MessageType::CommandRequest && req.payload == "LIST_PAPERS")
    {
        Message resp{MessageType::CommandResponse, "No papers yet."};
        return resp;
    }

    Message errorResp{MessageType::Error, "Unknown command"};
    return errorResp;
}

} // namespace osp::server


