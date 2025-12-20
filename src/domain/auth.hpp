#pragma once

#include "roles.hpp"
#include "user.hpp"

#include "common/types.hpp"

#include <functional>
#include <optional>
#include <cstddef>
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

// VFS 操作接口，用于解耦 AuthService 与具体 VFS 实现
struct VfsOperations
{
    std::function<bool(const std::string&)>                         createDirectory;
    std::function<bool(const std::string&, const std::string&)>     writeFile;
    std::function<std::optional<std::string>(const std::string&)>   readFile;
    std::function<bool(const std::string&)>                         removeFile;
    std::function<std::optional<std::string>(const std::string&)>   listDirectory;
};

// 认证服务：
// - 用户数据存储在 VFS 文件系统中（持久化）
// - 会话数据存储在内存中（临时）
class AuthService
{
public:
    AuthService();

    // 设置 VFS 操作接口，启用持久化
    void setVfsOperations(const VfsOperations& ops);

    // 检查是否已启用持久化
    [[nodiscard]] bool isPersistenceEnabled() const noexcept { return persistenceEnabled_; }

    // 从 VFS 加载用户数据
    bool loadUsers();

    // 添加一个用户（可用于初始化内置账号）
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

    // 当前活跃会话数（用于系统状态监控）
    [[nodiscard]] std::size_t sessionCount() const noexcept { return sessionsById_.size(); }

private:
    struct StoredUser
    {
        UserId      id{};
        std::string username;
        std::string password; // 当前为明文存储，仅用于教学示例
        Role        role{Role::Author};
    };

    // 保存单个用户到 VFS
    bool saveUser(const StoredUser& user);

    // 从 VFS 删除单个用户文件
    bool deleteUserFile(const std::string& username);

    // 保存 nextUserId 到 VFS
    bool saveNextUserId();

    // 加载 nextUserId 从 VFS
    bool loadNextUserId();

    std::unordered_map<std::string, StoredUser> usersByName_; // username -> StoredUser
    std::unordered_map<std::string, Session>    sessionsById_; // sessionId -> Session

    UserId nextUserId_{1};

    // VFS 操作接口
    VfsOperations vfsOps_;
    bool          persistenceEnabled_{false};

    // 用户数据存储路径
    static constexpr const char* kUsersDir = "/system/users";
    static constexpr const char* kNextUserIdPath = "/system/next_user_id";

    [[nodiscard]] std::string generateSessionId() const;
};

} // namespace osp::domain
