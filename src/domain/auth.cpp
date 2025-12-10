#include "auth.hpp"

#include <atomic>
#include <sstream>

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

} // namespace

AuthService::AuthService() = default;

void AuthService::addUser(const std::string& username,
                          const std::string& password,
                          Role               role)
{
    // 若用户名已存在，则简单覆盖（用于重复初始化场景）
    StoredUser u;
    u.id = nextUserId_++;
    u.username = username;
    u.password = password;
    u.role = role;

    usersByName_[username] = u;
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

std::string AuthService::generateSessionId() const
{
    // 当前实现直接委托给 makeSessionId，预留接口便于将来扩展。
    // 因为 makeSessionId 需要 userId，这个私有函数目前未被使用。
    // 保留该函数以便后续需要时复用/重构。
    return "unused-session-id";
}

} // namespace osp::domain



