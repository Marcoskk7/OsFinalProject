#include "server_app.hpp"

#include <sstream>

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

    // 使用阻塞式 TCP 服务器，循环处理来自多个客户端的请求
    osp::net::TcpServer tcpServer(port_);
    while (running_.load())
    {
        const bool ok = tcpServer.serveOnce([this](const osp::protocol::Message& req) {
            return handleRequest(req);
        });

        if (!ok)
        {
            osp::log(osp::LogLevel::Warn, "TcpServer::serveOnce failed, stopping server loop");
            break;
        }
    }

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

    if (req.type != MessageType::CommandRequest)
    {
        return {MessageType::Error, "Unsupported message type"};
    }

    std::istringstream iss(req.payload);
    std::string cmd;
    iss >> cmd;

    if (cmd == "LIST_PAPERS")
    {
        Message resp{MessageType::CommandResponse, "No papers yet."};
        return resp;
    }
    else if (cmd == "MKDIR")
    {
        std::string path;
        iss >> path;
        if (path.empty())
        {
            return {MessageType::Error, "MKDIR: missing path"};
        }

        bool ok = vfs_.createDirectory(path);
        return ok ? Message{MessageType::CommandResponse, "MKDIR ok: " + path}
                  : Message{MessageType::Error, "MKDIR failed: " + path};
    }
    else if (cmd == "WRITE")
    {
        std::string path;
        iss >> path;
        if (path.empty())
        {
            return {MessageType::Error, "WRITE: missing path"};
        }
        std::string content;
        std::getline(iss, content);
        if (!content.empty() && content.front() == ' ')
        {
            content.erase(content.begin());
        }

        bool ok = vfs_.writeFile(path, content);
        return ok ? Message{MessageType::CommandResponse, "WRITE ok: " + path}
                  : Message{MessageType::Error, "WRITE failed: " + path};
    }
    else if (cmd == "READ")
    {
        std::string path;
        iss >> path;
        if (path.empty())
        {
            return {MessageType::Error, "READ: missing path"};
        }

        auto data = vfs_.readFile(path);
        if (!data)
        {
            return {MessageType::Error, "READ failed: " + path};
        }
        return {MessageType::CommandResponse, *data};
    }
    else if (cmd == "RM")
    {
        std::string path;
        iss >> path;
        if (path.empty())
        {
            return {MessageType::Error, "RM: missing path"};
        }

        bool ok = vfs_.removeFile(path);
        return ok ? Message{MessageType::CommandResponse, "RM ok: " + path}
                  : Message{MessageType::Error, "RM failed: " + path};
    }
    else if (cmd == "RMDIR")
    {
        std::string path;
        iss >> path;
        if (path.empty())
        {
            return {MessageType::Error, "RMDIR: missing path"};
        }

        bool ok = vfs_.removeDirectory(path);
        return ok ? Message{MessageType::CommandResponse, "RMDIR ok: " + path}
                  : Message{MessageType::Error, "RMDIR failed (maybe not empty?): " + path};
    }
    else if (cmd == "LIST")
    {
        std::string path;
        iss >> path;
        if (path.empty())
        {
            path = "/";
        }

        auto listing = vfs_.listDirectory(path);
        if (!listing)
        {
            return {MessageType::Error, "LIST failed: " + path};
        }
        return {MessageType::CommandResponse, *listing};
    }

    return {MessageType::Error, "Unknown command: " + cmd};
}

} // namespace osp::server


