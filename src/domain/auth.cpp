#include "auth.hpp"

#include <atomic>
#include <sstream>
#include <vector>

namespace osp::domain
{

namespace
{
// 用简单的自增计数器生成 session id，足够满足教学与单进程演示场景。
std::string makeSessionId(UserId userId)
{
    static std::atomic<std::uint64_t> counter{1};
    std::ostringstream                 oss;
    oss << "sess-" << userId << "-" << counter.fetch_add(1);
    return oss.str();
}

// Role 转字符串
std::string roleToString(Role role)
{
    switch (role)
    {
    case Role::Author: return "Author";
    case Role::Reviewer: return "Reviewer";
    case Role::Editor: return "Editor";
    case Role::Admin: return "Admin";
    }
    return "Author";
}

// 字符串转 Role
Role stringToRole(const std::string& s)
{
    if (s == "Reviewer") return Role::Reviewer;
    if (s == "Editor") return Role::Editor;
    if (s == "Admin") return Role::Admin;
    return Role::Author;
}

} // namespace

AuthService::AuthService() = default;

void AuthService::setVfsOperations(const VfsOperations& ops)
{
    vfsOps_ = ops;
    persistenceEnabled_ = true;
}

bool AuthService::loadUsers()
{
    if (!persistenceEnabled_)
    {
        return false;
    }

    // 确保用户目录存在
    vfsOps_.createDirectory("/system");
    vfsOps_.createDirectory(kUsersDir);

    // 加载 nextUserId
    loadNextUserId();

    // 列出用户目录
    auto listing = vfsOps_.listDirectory(kUsersDir);
    if (!listing)
    {
        return true; // 目录为空或不存在，正常情况
    }

    std::stringstream ss(*listing);
    std::string       entry;

    while (std::getline(ss, entry))
    {
        if (entry.empty() || entry.back() == '/')
        {
            continue; // 跳过目录
        }

        // 移除 .txt 后缀得到用户名
        if (entry.size() > 4 && entry.substr(entry.size() - 4) == ".txt")
        {
            std::string username = entry.substr(0, entry.size() - 4);
            std::string userPath = std::string(kUsersDir) + "/" + entry;

            auto userData = vfsOps_.readFile(userPath);
            if (!userData)
            {
                continue;
            }

            // 解析用户数据：id\npassword\nrole
            std::stringstream uss(*userData);
            std::string       idStr;
            std::string       password;
            std::string       roleStr;

            if (!std::getline(uss, idStr) || !std::getline(uss, password) || !std::getline(uss, roleStr))
            {
                continue;
            }

            StoredUser user;
            try
            {
                user.id = static_cast<UserId>(std::stoul(idStr));
            }
            catch (...)
            {
                continue;
            }
            user.username = username;
            user.password = password;
            user.role = stringToRole(roleStr);

            usersByName_[username] = user;

            // 确保 nextUserId_ 大于所有已加载的用户 ID
            if (user.id >= nextUserId_)
            {
                nextUserId_ = user.id + 1;
            }
        }
    }

    return true;
}

bool AuthService::saveUser(const StoredUser& user)
{
    if (!persistenceEnabled_)
    {
        return true; // 未启用持久化，静默成功
    }

    // 确保目录存在
    vfsOps_.createDirectory("/system");
    vfsOps_.createDirectory(kUsersDir);

    std::string userPath = std::string(kUsersDir) + "/" + user.username + ".txt";

    // 用户数据格式：id\npassword\nrole
    std::ostringstream oss;
    oss << user.id << "\n"
        << user.password << "\n"
        << roleToString(user.role);

    return vfsOps_.writeFile(userPath, oss.str());
}

bool AuthService::deleteUserFile(const std::string& username)
{
    if (!persistenceEnabled_)
    {
        return true;
    }

    std::string userPath = std::string(kUsersDir) + "/" + username + ".txt";
    return vfsOps_.removeFile(userPath);
}

bool AuthService::saveNextUserId()
{
    if (!persistenceEnabled_)
    {
        return true;
    }

    vfsOps_.createDirectory("/system");
    return vfsOps_.writeFile(kNextUserIdPath, std::to_string(nextUserId_));
}

bool AuthService::loadNextUserId()
{
    if (!persistenceEnabled_)
    {
        return true;
    }

    auto data = vfsOps_.readFile(kNextUserIdPath);
    if (!data)
    {
        return false;
    }

    try
    {
        nextUserId_ = static_cast<UserId>(std::stoul(*data));
    }
    catch (...)
    {
        return false;
    }

    return true;
}

void AuthService::addUser(const std::string& username,
                          const std::string& password,
                          Role               role)
{
    // 检查用户是否已存在
    auto it = usersByName_.find(username);
    if (it != usersByName_.end())
    {
        // 用户已存在，更新密码和角色
        it->second.password = password;
        it->second.role = role;
        saveUser(it->second);
        return;
    }

    // 创建新用户
    StoredUser u;
    u.id = nextUserId_++;
    u.username = username;
    u.password = password;
    u.role = role;

    usersByName_[username] = u;

    // 持久化
    saveUser(u);
    saveNextUserId();
}

std::optional<Session> AuthService::login(const osp::Credentials& credentials)
{
    auto it = usersByName_.find(credentials.username);
    if (it == usersByName_.end())
    {
        return std::nullopt;
    }

    const StoredUser& u = it->second;
    if (u.password != credentials.password)
    {
        return std::nullopt;
    }

    Session s;
    s.id = makeSessionId(u.id);
    s.userId = u.id;
    s.role = u.role;
    s.username = u.username;

    sessionsById_[s.id] = s;
    return s;
}

std::optional<UserId> AuthService::getUserId(const std::string& username) const
{
    auto it = usersByName_.find(username);
    if (it == usersByName_.end())
    {
        return std::nullopt;
    }
    return it->second.id;
}

std::optional<Session> AuthService::validateSession(const std::string& sessionId) const
{
    auto it = sessionsById_.find(sessionId);
    if (it == sessionsById_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

bool AuthService::removeUser(const std::string& username)
{
    auto it = usersByName_.find(username);
    if (it == usersByName_.end())
    {
        return false;
    }

    // 从 VFS 删除用户文件
    deleteUserFile(username);

    usersByName_.erase(it);
    return true;
}

bool AuthService::updateUserRole(const std::string& username, Role role)
{
    auto it = usersByName_.find(username);
    if (it == usersByName_.end())
    {
        return false;
    }

    it->second.role = role;

    // 持久化更新
    saveUser(it->second);

    return true;
}

bool AuthService::resetUserPassword(const std::string& username, const std::string& newPassword)
{
    auto it = usersByName_.find(username);
    if (it == usersByName_.end())
    {
        return false;
    }

    it->second.password = newPassword;

    // 持久化更新
    saveUser(it->second);

    return true;
}

std::vector<User> AuthService::getAllUsers() const
{
    std::vector<User> users;
    users.reserve(usersByName_.size());
    
    for (const auto& [username, storedUser] : usersByName_)
    {
        users.emplace_back(storedUser.id, storedUser.username, storedUser.role);
    }
    
    return users;
}

std::string AuthService::generateSessionId() const
{
    // 当前实现直接委托给 makeSessionId，预留接口便于将来扩展。
    // 因为 makeSessionId 需要 userId，这个私有函数目前未被使用。
    // 保留该函数以便后续需要时复用/重构。
    return "unused-session-id";
}

} // namespace osp::domain
