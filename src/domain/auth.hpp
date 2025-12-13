#pragma once

#include "roles.hpp"
#include "user.hpp"

#include "common/types.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace osp::domain
{

// 表示一次登录会话（Session）
struct Session
{
    std::string id;      // 会话 ID，用于客户端在后续请求中携带
    UserId      userId;  // 用户 ID
    Role        role;    // 用户角色
    std::string username;
};

// 简化版认证服务：
// - 使用内存中的用户表与 Session 表；
// - 后续可以替换为基于自定义文件系统的持久化用户存储。
class AuthService
{
public:
    AuthService();

    // 添加一个用户（可用于初始化内置账号）。
    void addUser(const std::string& username,
                 const std::string& password,
                 Role               role);

    // 删除一个用户
    bool removeUser(const std::string& username);

    // 更新用户角色
    bool updateUserRole(const std::string& username, Role role);

    // 重置用户密码
    bool resetUserPassword(const std::string& username, const std::string& newPassword);

    // 获取所有用户列表
    std::vector<User> getAllUsers() const;

    // 尝试使用凭证登录，成功则返回新的 Session（并记录在服务内部），失败返回 std::nullopt。
    std::optional<Session> login(const osp::Credentials& credentials);

    // 根据用户名查找用户 ID（用于分配审稿人等场景）
    std::optional<UserId> getUserId(const std::string& username) const;

    // 根据会话 ID 查找已有 Session，用于后续请求的鉴权。
    std::optional<Session> validateSession(const std::string& sessionId) const;

private:
    struct StoredUser
    {
        UserId      id{};
        std::string username;
        std::string password; // 当前为明文存储，仅用于教学示例
        Role        role{Role::Author};
    };

    std::unordered_map<std::string, StoredUser> usersByName_; // username -> StoredUser
    std::unordered_map<std::string, Session>    sessionsById_; // sessionId -> Session

    UserId nextUserId_{1};

    [[nodiscard]] std::string generateSessionId() const;
};

} // namespace osp::domain



