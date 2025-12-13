#pragma once

#include "common/protocol.hpp"

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

    // 启动交互式命令行循环。
    // - 支持 LOGIN 命令获取会话 ID（session）；
    // - 对后续业务命令自动在发送前附加 "SESSION <id> CMD ..." 前缀，用户无需手动输入 session。
    void run();

private:
    // 根据当前 sessionId 状态，将用户输入的一行命令封装为实际发送的 payload。
    // - 未登录或无需携带会话时：直接返回原始行（例如 PING / LOGIN）。
    // - 已登录且为普通业务命令时：自动封装为 "SESSION <sessionId> CMD <原始命令行>"。
    [[nodiscard]] std::string buildPayload(const std::string& line) const;

    // 当客户端发送 LOGIN 命令并收到成功响应时，从响应 payload 中解析并保存新的 sessionId。
    void handleLoginResponse(const std::string& requestLine,
                             const osp::protocol::Message& resp);

    // 打印通用指引（PING/LOGIN/退出等）。
    void printGeneralGuide() const;

    // 登录成功后，基于当前角色打印可用命令提示。
    void printRoleGuide() const;

    // 处理管理员数字菜单的输入与多步流程。
    bool handleAdminMenuInput(const std::string& line);
    void printAdminNumericMenu() const;

    // 处理编辑数字菜单的输入与多步流程。
    bool handleEditorMenuInput(const std::string& line);
    void printEditorNumericMenu() const;

private:
    std::string    host_;
    unsigned short port_{};
    std::string    sessionId_;          // 当前会话 ID，空字符串表示未登录
    std::string    currentUser_;        // 当前登录用户名
    std::string    currentRole_;        // 当前登录角色（Admin / Editor / ...）
    std::string    currentPath_ = "/";  // 客户端维护的“当前目录”（仅影响默认 LIST 等命令）

    // 管理员数字菜单的临时状态
    enum class AdminWizard
    {
        None,
        AddReviewerAskName,
        AddReviewerAskPassword,
        RemoveUserAskName,
        UpdateRoleAskName,
        UpdateRoleAskRole,
        ResetPwdAskName,
        ResetPwdAskNewPwd,
        BackupAskPath,
        RestoreAskPath,
        PostAddPrompt,
        PostRemovePrompt,
        PostUpdatePrompt,
        PostResetPwdPrompt,
        PostBackupPrompt,
        PostRestorePrompt
    };
    AdminWizard  adminWizard_ = AdminWizard::None;
    std::string  tempUsername_;
    std::string  tempPassword_;
    std::string  tempRole_;
    std::string  tempPath_;

    // 编辑数字菜单的临时状态
    enum class EditorWizard
    {
        None,
        AssignAskPaperId,
        AssignAskReviewer,
        ViewAskPaperId,
        DecideAskPaperId,
        DecideAskDecision,
        PostAssignPrompt,
        PostViewPrompt,
        PostDecidePrompt
    };
    EditorWizard editorWizard_ = EditorWizard::None;
    std::string  tempPaperId_;
    std::string  tempDecision_;
};

} // namespace osp::client


