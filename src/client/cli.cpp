#include "cli.hpp"

#include "client/net/tcp_client.hpp"
#include "common/logger.hpp"
#include "common/protocol.hpp"

#include <iostream>
#include <string>

namespace osp::client
{

void Cli::run()
{
    osp::log(osp::LogLevel::Info, "Client CLI started. Type commands or 'quit' to exit.");

    for (;;)
    {
        std::cout << "> " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line))
        {
            // EOF 或输入流错误，退出客户端
            break;
        }

        if (line == "quit" || line == "exit")
        {
            osp::log(osp::LogLevel::Info, "Client exiting by user command");
            break;
        }

        if (line.empty())
        {
            continue;
        }

        // 当前服务器只识别 LIST_PAPERS 命令，其它命令会返回 Error。
        osp::protocol::Message req{osp::protocol::MessageType::CommandRequest, line};
        osp::log(osp::LogLevel::Info, "Send request: " + req.payload + " to " + host_ + ":" + std::to_string(port_));

        osp::net::TcpClient tcpClient(host_, port_);
        auto resp = tcpClient.request(req);
        if (!resp)
        {
            osp::log(osp::LogLevel::Error, "Failed to receive response from server");
            continue;
        }

        osp::log(osp::LogLevel::Info, "Received response: " + resp->payload);
    }
}

} // namespace osp::client


