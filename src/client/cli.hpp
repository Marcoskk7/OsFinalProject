#pragma once

#include <string>

namespace osp::client
{

// 命令行界面（CLI）入口的简单封装
class Cli
{
public:
    Cli(std::string serverHost, unsigned short serverPort)
        : host_(std::move(serverHost))
        , port_(serverPort)
    {
    }

    // 初版：本地模拟一次“登录 + 查看论文列表”的请求流程
    void run();

private:
    std::string host_;
    unsigned short port_;
};

} // namespace osp::client


