#include "cli.hpp"

#include "client/net/tcp_client.hpp"
#include "common/logger.hpp"
#include "common/protocol.hpp"

namespace osp::client
{

void Cli::run()
{
    osp::log(osp::LogLevel::Info, "Client connecting to " + host_ + ":" + std::to_string(port_));

    osp::protocol::Message req{osp::protocol::MessageType::CommandRequest, "LIST_PAPERS"};
    osp::log(osp::LogLevel::Info, "Send request: " + req.payload);

    osp::net::TcpClient tcpClient(host_, port_);
    auto resp = tcpClient.request(req);
    if (!resp)
    {
        osp::log(osp::LogLevel::Error, "Failed to receive response from server");
        return;
    }

    osp::log(osp::LogLevel::Info, "Received response: " + resp->payload);
}

} // namespace osp::client


