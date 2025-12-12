#include "cli.hpp"

#include "client/net/tcp_client.hpp"
#include "common/logger.hpp"

#include <cctype>
#include <iostream>
#include <string>
#include <string_view>

namespace osp::client
{

namespace
{

bool isLoginCommand(std::string_view line)
{
    // 判断是否以 "LOGIN" 或 "login" 开头（忽略前导空格，大小写不敏感）
    std::size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
    {
        ++i;
    }
    if (i + 5 > line.size())
    {
        return false;
    }

    const char* kw = "LOGIN";
    for (std::size_t k = 0; k < 5; ++k)
    {
        if (std::toupper(static_cast<unsigned char>(line[i + k])) != kw[k])
        {
            return false;
        }
    }

    // 后面要么是空白、要么行结束
    if (i + 5 < line.size())
    {
        char c = line[i + 5];
        if (!std::isspace(static_cast<unsigned char>(c)))
        {
            return false;
        }
    }
    return true;
}

bool isCdCommand(std::string_view line)
{
    // 判断是否以 "CD" 或 "cd" 开头（忽略前导空格，大小写不敏感）
    std::size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
    {
        ++i;
    }
    if (i + 2 > line.size())
    {
        return false;
    }

    const char* kw = "CD";
    for (std::size_t k = 0; k < 2; ++k)
    {
        if (std::toupper(static_cast<unsigned char>(line[i + k])) != kw[k])
        {
            return false;
        }
    }

    if (i + 2 < line.size())
    {
        char c = line[i + 2];
        if (!std::isspace(static_cast<unsigned char>(c)))
        {
            return false;
        }
    }
    return true;
}

} // namespace

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

        // 特殊处理 CD 命令：仅在客户端更新当前目录，并向服务器发送一次 LIST 进行合法性校验。
        if (isCdCommand(line))
        {
            osp::protocol::Command cmd = osp::protocol::parseCommandLine(line);
            if (cmd.args.empty())
            {
                std::cout << "CD: missing path\n";
                continue;
            }

            const std::string& target = cmd.args[0];

            // 计算新的工作目录（不支持 "." / ".." 等特殊路径，仅支持简单的绝对或相对路径）。
            std::string newPath;
            if (!target.empty() && target[0] == '/')
            {
                newPath = target;
            }
            else
            {
                if (currentPath_ == "/" || currentPath_.empty())
                {
                    newPath = "/" + target;
                }
                else
                {
                    newPath = currentPath_ + "/" + target;
                }
            }

            // 通过向服务器发送 LIST newPath 来验证该目录是否存在且可访问。
            const std::string listLine = "LIST " + newPath;
            const std::string payload = buildPayload(listLine);
            osp::protocol::Message req{osp::protocol::MessageType::CommandRequest, payload};

            osp::log(osp::LogLevel::Info,
                     "Send request: " + req.payload + " to " + host_ + ":" + std::to_string(port_));

            osp::net::TcpClient tcpClient(host_, port_);
            auto                resp = tcpClient.request(req);
            if (!resp)
            {
                osp::log(osp::LogLevel::Error, "CD: failed to contact server");
                std::cout << "CD: failed to contact server\n";
                continue;
            }

            if (resp->type == osp::protocol::MessageType::Error)
            {
                std::cout << "CD failed: " << resp->payload << '\n';
                continue;
            }

            // LIST 成功，更新当前目录，但不打印目录内容，避免与 LIST 命令的语义混淆。
            currentPath_ = newPath;
            std::cout << "Current directory: " << currentPath_ << '\n';
            continue;
        }

        const std::string payload = buildPayload(line);
        osp::protocol::Message req{osp::protocol::MessageType::CommandRequest, payload};

        osp::log(osp::LogLevel::Info,
                 "Send request: " + req.payload + " to " + host_ + ":" + std::to_string(port_));

        osp::net::TcpClient tcpClient(host_, port_);
        auto                resp = tcpClient.request(req);
        if (!resp)
        {
            osp::log(osp::LogLevel::Error, "Failed to receive response from server");
            continue;
        }

        // 日志只输出简要信息，实际结果由标准输出打印，避免重复显示内容。
        osp::log(osp::LogLevel::Info, "Received response from server");
        std::cout << resp->payload << '\n';

        // 如果这是一次 LOGIN 调用并且服务器返回成功的会话信息，则更新本地 sessionId_
        if (isLoginCommand(line))
        {
            handleLoginResponse(line, *resp);
        }
    }
}

std::string Cli::buildPayload(const std::string& line) const
{
    // LOGIN 始终不自动附加 SESSION 前缀，防止递归/混淆；
    // 其他命令在已登录时自动附加 "SESSION <sessionId> CMD ..." 前缀。
    if (isLoginCommand(line))
    {
        return line;
    }

    osp::protocol::Command cmd = osp::protocol::parseCommandLine(line);
    if (cmd.name.empty())
    {
        return line; // 解析失败则回退为原始行
    }

    // 当用户输入简单的 "LIST" 时，自动在客户端侧使用当前目录作为参数。
    if (cmd.name == "LIST" && cmd.rawArgs.empty())
    {
        cmd.rawArgs.clear();
        cmd.args.clear();
        cmd.rawArgs = currentPath_;
        cmd.args.push_back(currentPath_);
    }

    if (!sessionId_.empty())
    {
        cmd.sessionId = sessionId_;
        return osp::protocol::buildCommandPayload(cmd);
    }

    // 未登录时，保持原始格式（不添加 SESSION/CMD 前缀）
    return osp::protocol::buildCommandPayload(cmd);
}

void Cli::handleLoginResponse(const std::string&               requestLine,
                              const osp::protocol::Message& resp)
{
    if (resp.type != osp::protocol::MessageType::CommandResponse)
    {
        return;
    }

    // 预期格式： "SESSION <id> USER <username> ROLE <RoleName>"
    const std::string& p = resp.payload;
    constexpr const char* kPrefix = "SESSION ";

    if (p.rfind(kPrefix, 0) != 0)
    {
        // 不是成功的登录响应，可能是错误信息
        return;
    }

    const std::size_t idStart = std::char_traits<char>::length(kPrefix);
    auto              idEnd = p.find(' ', idStart);
    std::string       newId =
        (idEnd == std::string::npos) ? p.substr(idStart) : p.substr(idStart, idEnd - idStart);

    if (newId.empty())
    {
        return;
    }

    sessionId_ = newId;
    osp::log(osp::LogLevel::Info, "Updated session id from LOGIN '" + requestLine
                                      + "': " + sessionId_);

    // 解析角色并打印帮助信息
    // 格式: SESSION <id> USER <username> ROLE <RoleName>
    constexpr const char* kRolePrefix = " ROLE ";
    auto rolePos = p.find(kRolePrefix);
    if (rolePos != std::string::npos)
    {
        std::string role = p.substr(rolePos + std::char_traits<char>::length(kRolePrefix));
        // 去除可能存在的尾部空白
        while (!role.empty() && std::isspace(role.back()))
        {
            role.pop_back();
        }
        printHelpForRole(role);
    }
}

void Cli::printHelpForRole(const std::string& role) const
{
    std::cout << "\n==================================================\n";
    std::cout << " Login Successful! Welcome, " << role << ".\n";
    std::cout << "==================================================\n";
    std::cout << "Available commands for your role:\n\n";

    if (role == "Author")
    {
        std::cout << "  SUBMIT <Title> <Content...>  - Upload a new paper\n";
        std::cout << "                                 Example: SUBMIT MyPaper This is the content\n";
        std::cout << "  LIST_PAPERS                  - List your submitted papers\n";
        std::cout << "  GET_PAPER <PaperID>          - View paper details\n";
    }
    else if (role == "Reviewer")
    {
        std::cout << "  LIST_PAPERS                  - List assigned papers\n";
        std::cout << "  GET_PAPER <PaperID>          - View paper details\n";
        std::cout << "  REVIEW <PaperID> <Decision> <Comments...>\n";
        std::cout << "                                 Decisions: ACCEPT, REJECT, MINOR, MAJOR\n";
    }
    else if (role == "Editor")
    {
        std::cout << "  LIST_PAPERS                  - List all papers\n";
        std::cout << "  GET_PAPER <PaperID>          - View paper details\n";
        std::cout << "  ASSIGN <PaperID> <User>      - Assign a reviewer to a paper\n";
        std::cout << "  LIST_REVIEWS <PaperID>       - View all reviews for a paper\n";
        std::cout << "  DECISION <PaperID> <Result>  - Make final decision (ACCEPT/REJECT)\n";
    }
    else if (role == "Admin")
    {
        std::cout << "  MKDIR <Path>                 - Create directory\n";
        std::cout << "  LIST <Path>                  - List directory\n";
        std::cout << "  WRITE <Path> <Content>       - Write file\n";
        std::cout << "  READ <Path>                  - Read file\n";
        std::cout << "  RM <Path>                    - Remove file\n";
    }

    std::cout << "\nGeneral Commands:\n";
    std::cout << "  PING                         - Test connection\n";
    std::cout << "  CD <Path>                    - Change current directory\n";
    std::cout << "  quit                         - Exit client\n";
    std::cout << "==================================================\n\n";
}

} // namespace osp::client


