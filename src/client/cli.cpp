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
    printGeneralGuide();

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

        // 管理员数字菜单 / 向导处理（优先于普通命令）
        if (currentRole_ == "Admin")
        {
            if (handleAdminMenuInput(line))
            {
                continue;
            }
        }
        else if (currentRole_ == "Editor")
        {
            if (handleEditorMenuInput(line))
            {
                continue;
            }
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
            printRoleGuide();
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

    // 解析 USER 与 ROLE（格式：SESSION <id> USER <username> ROLE <Role>）
    auto userPos = p.find(" USER ");
    auto rolePos = p.find(" ROLE ");
    if (userPos != std::string::npos && rolePos != std::string::npos && rolePos > userPos + 6)
    {
        currentUser_ = p.substr(userPos + 6, rolePos - (userPos + 6));
        currentRole_ = p.substr(rolePos + 6);
    }

    osp::log(osp::LogLevel::Info, "Updated session id from LOGIN '" + requestLine
                                      + "': " + sessionId_);
}

void Cli::printGeneralGuide() const
{
    std::cout << "=== 指引 ===\n";
    std::cout << "基础：PING（连通性），LOGIN <user> <pass>，quit/exit 退出\n";
    std::cout << "文件：LIST [path] | MKDIR <path> | WRITE <path> <content> | READ <path> | RM <path> | RMDIR <path> | CD <path>\n";
    std::cout << "内置账号（用户名=密码）：admin / author / reviewer / editor\n";
    std::cout << "----------------\n";
}

void Cli::printRoleGuide() const
{
    if (sessionId_.empty())
    {
        return;
    }

    std::cout << "当前用户: " << (currentUser_.empty() ? "(未知)" : currentUser_);
    if (!currentRole_.empty())
    {
        std::cout << "  角色: " << currentRole_;
    }
    std::cout << "\n";

    if (currentRole_ == "Editor")
    {
        std::cout << "[Editor 可用命令]\n";
        std::cout << "1) 指派审稿人: ASSIGN_REVIEWER <paper_id> <reviewer_username>\n";
        std::cout << "2) 查看审稿状态: VIEW_REVIEW_STATUS <paper_id>\n";
        std::cout << "3) 最终决定: MAKE_FINAL_DECISION <paper_id> <decision>\n";
        printEditorNumericMenu();
    }
    else if (currentRole_ == "Admin")
    {
        std::cout << "[Admin 可用命令]\n";
        std::cout << "MANAGE_USERS LIST | ADD <u> <p> <Role> | UPDATE_ROLE <u> <Role> | REMOVE <u> | RESET_PASSWORD <u> <new>\n";
        std::cout << "BACKUP <path> | RESTORE <path> | VIEW_SYSTEM_STATUS\n";
        printAdminNumericMenu();
    }
    else if (currentRole_ == "Reviewer")
    {
        std::cout << "[Reviewer 占位命令]\n";
        std::cout << "(当前无专属命令，可使用文件/通用命令)\n";
    }
    else if (currentRole_ == "Author")
    {
        std::cout << "[Author 占位命令]\n";
        std::cout << "(当前无专属命令，可使用文件/通用命令)\n";
    }
    std::cout << "----------------\n";
}

void Cli::printAdminNumericMenu() const
{
    std::cout << "[Admin 数字菜单]\n";
    std::cout << "1) 列出用户\n";
    std::cout << "2) 添加 Reviewer\n";
    std::cout << "3) 删除用户\n";
    std::cout << "4) 更新用户角色\n";
    std::cout << "5) 重置用户密码\n";
    std::cout << "6) 备份\n";
    std::cout << "7) 恢复\n";
    std::cout << "8) 查看系统状态\n";
    std::cout << "(直接输入数字开始操作，或输入原始命令也可)\n";
    std::cout << "----------------\n";
}

bool Cli::handleAdminMenuInput(const std::string& line)
{
    auto trim = [](const std::string& s) {
        std::size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string{};
        std::size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    };

    const std::string t = trim(line);

    // 如果处于向导状态，消费输入
    if (adminWizard_ != AdminWizard::None)
    {
        switch (adminWizard_)
        {
        case AdminWizard::AddReviewerAskName:
            tempUsername_ = t;
            std::cout << "输入密码: ";
            adminWizard_ = AdminWizard::AddReviewerAskPassword;
            return true;
        case AdminWizard::AddReviewerAskPassword:
        {
            tempPassword_ = t.empty() ? "123456" : t;
            const std::string cmd = "MANAGE_USERS ADD " + tempUsername_ + " " + tempPassword_ + " Reviewer";
            const std::string payload = buildPayload(cmd);
            osp::protocol::Message req{osp::protocol::MessageType::CommandRequest, payload};
            osp::net::TcpClient tcpClient(host_, port_);
            if (auto resp = tcpClient.request(req))
            {
                std::cout << resp->payload << '\n';
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续添加 Reviewer，m 返回管理员菜单，其他退出向导: ";
            adminWizard_ = AdminWizard::PostAddPrompt;
            return true;
        }
        case AdminWizard::RemoveUserAskName:
        {
            const std::string cmd = "MANAGE_USERS REMOVE " + t;
            const std::string payload = buildPayload(cmd);
            osp::protocol::Message req{osp::protocol::MessageType::CommandRequest, payload};
            osp::net::TcpClient tcpClient(host_, port_);
            if (auto resp = tcpClient.request(req))
            {
                std::cout << resp->payload << '\n';
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续删除，m 返回管理员菜单，其他退出向导: ";
            adminWizard_ = AdminWizard::PostRemovePrompt;
            return true;
        }
        case AdminWizard::UpdateRoleAskName:
            tempUsername_ = t;
            std::cout << "输入角色（Author/Reviewer/Editor/Admin）: ";
            adminWizard_ = AdminWizard::UpdateRoleAskRole;
            return true;
        case AdminWizard::UpdateRoleAskRole:
        {
            tempRole_ = t;
            const std::string cmd = "MANAGE_USERS UPDATE_ROLE " + tempUsername_ + " " + tempRole_;
            const std::string payload = buildPayload(cmd);
            osp::protocol::Message req{osp::protocol::MessageType::CommandRequest, payload};
            osp::net::TcpClient tcpClient(host_, port_);
            if (auto resp = tcpClient.request(req))
            {
                std::cout << resp->payload << '\n';
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续更新角色，m 返回管理员菜单，其他退出向导: ";
            adminWizard_ = AdminWizard::PostUpdatePrompt;
            return true;
        }
        case AdminWizard::ResetPwdAskName:
            tempUsername_ = t;
            std::cout << "输入新密码: ";
            adminWizard_ = AdminWizard::ResetPwdAskNewPwd;
            return true;
        case AdminWizard::ResetPwdAskNewPwd:
        {
            tempPassword_ = t;
            const std::string cmd = "MANAGE_USERS RESET_PASSWORD " + tempUsername_ + " " + tempPassword_;
            const std::string payload = buildPayload(cmd);
            osp::protocol::Message req{osp::protocol::MessageType::CommandRequest, payload};
            osp::net::TcpClient tcpClient(host_, port_);
            if (auto resp = tcpClient.request(req))
            {
                std::cout << resp->payload << '\n';
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续重置密码，m 返回管理员菜单，其他退出向导: ";
            adminWizard_ = AdminWizard::PostResetPwdPrompt;
            return true;
        }
        case AdminWizard::BackupAskPath:
        {
            const std::string cmd = "BACKUP " + t;
            const std::string payload = buildPayload(cmd);
            osp::protocol::Message req{osp::protocol::MessageType::CommandRequest, payload};
            osp::net::TcpClient tcpClient(host_, port_);
            if (auto resp = tcpClient.request(req))
            {
                std::cout << resp->payload << '\n';
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续备份，m 返回管理员菜单，其他退出向导: ";
            adminWizard_ = AdminWizard::PostBackupPrompt;
            return true;
        }
        case AdminWizard::RestoreAskPath:
        {
            const std::string cmd = "RESTORE " + t;
            const std::string payload = buildPayload(cmd);
            osp::protocol::Message req{osp::protocol::MessageType::CommandRequest, payload};
            osp::net::TcpClient tcpClient(host_, port_);
            if (auto resp = tcpClient.request(req))
            {
                std::cout << resp->payload << '\n';
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续恢复，m 返回管理员菜单，其他退出向导: ";
            adminWizard_ = AdminWizard::PostRestorePrompt;
            return true;
        }
        case AdminWizard::PostAddPrompt:
            if (t == "c" || t == "C")
            {
                adminWizard_ = AdminWizard::AddReviewerAskName;
                std::cout << "添加 Reviewer，输入用户名: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                adminWizard_ = AdminWizard::None;
                printAdminNumericMenu();
                return true;
            }
            adminWizard_ = AdminWizard::None;
            return true;
        case AdminWizard::PostRemovePrompt:
            if (t == "c" || t == "C")
            {
                adminWizard_ = AdminWizard::RemoveUserAskName;
                std::cout << "删除用户，输入用户名: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                adminWizard_ = AdminWizard::None;
                printAdminNumericMenu();
                return true;
            }
            adminWizard_ = AdminWizard::None;
            return true;
        case AdminWizard::PostUpdatePrompt:
            if (t == "c" || t == "C")
            {
                adminWizard_ = AdminWizard::UpdateRoleAskName;
                std::cout << "更新角色，输入用户名: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                adminWizard_ = AdminWizard::None;
                printAdminNumericMenu();
                return true;
            }
            adminWizard_ = AdminWizard::None;
            return true;
        case AdminWizard::PostResetPwdPrompt:
            if (t == "c" || t == "C")
            {
                adminWizard_ = AdminWizard::ResetPwdAskName;
                std::cout << "重置密码，输入用户名: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                adminWizard_ = AdminWizard::None;
                printAdminNumericMenu();
                return true;
            }
            adminWizard_ = AdminWizard::None;
            return true;
        case AdminWizard::PostBackupPrompt:
            if (t == "c" || t == "C")
            {
                adminWizard_ = AdminWizard::BackupAskPath;
                std::cout << "备份路径: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                adminWizard_ = AdminWizard::None;
                printAdminNumericMenu();
                return true;
            }
            adminWizard_ = AdminWizard::None;
            return true;
        case AdminWizard::PostRestorePrompt:
            if (t == "c" || t == "C")
            {
                adminWizard_ = AdminWizard::RestoreAskPath;
                std::cout << "恢复路径: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                adminWizard_ = AdminWizard::None;
                printAdminNumericMenu();
                return true;
            }
            adminWizard_ = AdminWizard::None;
            return true;
        default: break;
        }
    }

    // 非向导状态，识别数字入口
    if (t == "1")
    {
        const std::string cmd = "MANAGE_USERS LIST";
        const std::string payload = buildPayload(cmd);
        osp::protocol::Message req{osp::protocol::MessageType::CommandRequest, payload};
        osp::net::TcpClient tcpClient(host_, port_);
        if (auto resp = tcpClient.request(req))
        {
            std::cout << resp->payload << '\n';
        }
        else
        {
            std::cout << "发送失败\n";
        }
        return true;
    }
    if (t == "2")
    {
        adminWizard_ = AdminWizard::AddReviewerAskName;
        std::cout << "添加 Reviewer，输入用户名: ";
        return true;
    }
    if (t == "3")
    {
        adminWizard_ = AdminWizard::RemoveUserAskName;
        std::cout << "删除用户，输入用户名: ";
        return true;
    }
    if (t == "4")
    {
        adminWizard_ = AdminWizard::UpdateRoleAskName;
        std::cout << "更新角色，输入用户名: ";
        return true;
    }
    if (t == "5")
    {
        adminWizard_ = AdminWizard::ResetPwdAskName;
        std::cout << "重置密码，输入用户名: ";
        return true;
    }
    if (t == "6")
    {
        adminWizard_ = AdminWizard::BackupAskPath;
        std::cout << "备份路径: ";
        return true;
    }
    if (t == "7")
    {
        adminWizard_ = AdminWizard::RestoreAskPath;
        std::cout << "恢复路径: ";
        return true;
    }
    if (t == "8")
    {
        const std::string cmd = "VIEW_SYSTEM_STATUS";
        const std::string payload = buildPayload(cmd);
        osp::protocol::Message req{osp::protocol::MessageType::CommandRequest, payload};
        osp::net::TcpClient tcpClient(host_, port_);
        if (auto resp = tcpClient.request(req))
        {
            std::cout << resp->payload << '\n';
        }
        else
        {
            std::cout << "发送失败\n";
        }
        return true;
    }

    // 其他输入不处理，交给原有逻辑
    return false;
}

void Cli::printEditorNumericMenu() const
{
    std::cout << "[Editor 数字菜单]\n";
    std::cout << "1) 指派审稿人\n";
    std::cout << "2) 查看审稿状态\n";
    std::cout << "3) 最终决定\n";
    std::cout << "(直接输入数字开始操作，或输入原始命令也可)\n";
    std::cout << "----------------\n";
}

bool Cli::handleEditorMenuInput(const std::string& line)
{
    auto trim = [](const std::string& s) {
        std::size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string{};
        std::size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    };

    const std::string t = trim(line);

    // 如果处于向导状态，消费输入
    if (editorWizard_ != EditorWizard::None)
    {
        switch (editorWizard_)
        {
        case EditorWizard::AssignAskPaperId:
            tempPaperId_ = t;
            std::cout << "输入 reviewer 用户名: ";
            editorWizard_ = EditorWizard::AssignAskReviewer;
            return true;
        case EditorWizard::AssignAskReviewer:
        {
            const std::string cmd = "ASSIGN_REVIEWER " + tempPaperId_ + " " + t;
            const std::string payload = buildPayload(cmd);
            osp::protocol::Message req{osp::protocol::MessageType::CommandRequest, payload};
            osp::net::TcpClient tcpClient(host_, port_);
            if (auto resp = tcpClient.request(req))
            {
                std::cout << resp->payload << '\n';
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续指派，m 返回编辑菜单，其他退出向导: ";
            editorWizard_ = EditorWizard::PostAssignPrompt;
            return true;
        }
        case EditorWizard::ViewAskPaperId:
        {
            const std::string cmd = "VIEW_REVIEW_STATUS " + t;
            const std::string payload = buildPayload(cmd);
            osp::protocol::Message req{osp::protocol::MessageType::CommandRequest, payload};
            osp::net::TcpClient tcpClient(host_, port_);
            if (auto resp = tcpClient.request(req))
            {
                std::cout << resp->payload << '\n';
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续查看，m 返回编辑菜单，其他退出向导: ";
            editorWizard_ = EditorWizard::PostViewPrompt;
            return true;
        }
        case EditorWizard::DecideAskPaperId:
            tempPaperId_ = t;
            std::cout << "输入决定（例如 Accept/Reject）: ";
            editorWizard_ = EditorWizard::DecideAskDecision;
            return true;
        case EditorWizard::DecideAskDecision:
        {
            tempDecision_ = t;
            const std::string cmd = "MAKE_FINAL_DECISION " + tempPaperId_ + " " + tempDecision_;
            const std::string payload = buildPayload(cmd);
            osp::protocol::Message req{osp::protocol::MessageType::CommandRequest, payload};
            osp::net::TcpClient tcpClient(host_, port_);
            if (auto resp = tcpClient.request(req))
            {
                std::cout << resp->payload << '\n';
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续决策，m 返回编辑菜单，其他退出向导: ";
            editorWizard_ = EditorWizard::PostDecidePrompt;
            return true;
        }
        case EditorWizard::PostAssignPrompt:
            if (t == "c" || t == "C")
            {
                editorWizard_ = EditorWizard::AssignAskPaperId;
                std::cout << "指派审稿人，输入 paper_id: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                editorWizard_ = EditorWizard::None;
                printEditorNumericMenu();
                return true;
            }
            editorWizard_ = EditorWizard::None;
            return true;
        case EditorWizard::PostViewPrompt:
            if (t == "c" || t == "C")
            {
                editorWizard_ = EditorWizard::ViewAskPaperId;
                std::cout << "查看审稿状态，输入 paper_id: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                editorWizard_ = EditorWizard::None;
                printEditorNumericMenu();
                return true;
            }
            editorWizard_ = EditorWizard::None;
            return true;
        case EditorWizard::PostDecidePrompt:
            if (t == "c" || t == "C")
            {
                editorWizard_ = EditorWizard::DecideAskPaperId;
                std::cout << "最终决定，输入 paper_id: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                editorWizard_ = EditorWizard::None;
                printEditorNumericMenu();
                return true;
            }
            editorWizard_ = EditorWizard::None;
            return true;
        default: break;
        }
    }

    // 非向导状态，识别数字入口
    if (t == "1")
    {
        editorWizard_ = EditorWizard::AssignAskPaperId;
        std::cout << "指派审稿人，输入 paper_id: ";
        return true;
    }
    if (t == "2")
    {
        editorWizard_ = EditorWizard::ViewAskPaperId;
        std::cout << "查看审稿状态，输入 paper_id: ";
        return true;
    }
    if (t == "3")
    {
        editorWizard_ = EditorWizard::DecideAskPaperId;
        std::cout << "最终决定，输入 paper_id: ";
        return true;
    }

    return false; // 其他输入交给原有逻辑
}

} // namespace osp::client


