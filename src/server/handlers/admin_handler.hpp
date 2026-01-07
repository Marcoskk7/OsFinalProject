#pragma once

#include "common/protocol.hpp"
#include "domain/auth.hpp"
#include "server/filesystem/vfs.hpp"

#include <mutex>
#include <optional>

namespace osp::server
{

class AdminHandler
{
public:
    AdminHandler(osp::fs::Vfs& vfs, std::mutex& vfsMutex, osp::domain::AuthService& auth, std::mutex& authMutex)
        : vfs_(vfs)
        , vfsMutex_(vfsMutex)
        , auth_(auth)
        , authMutex_(authMutex)
    {
    }

    // 返回 std::nullopt 表示不处理该命令
    std::optional<osp::protocol::Message> tryHandle(const osp::protocol::Command& cmd,
                                                    const std::optional<osp::domain::Session>& maybeSession);

private:
    osp::fs::Vfs&             vfs_;
    std::mutex&               vfsMutex_;
    osp::domain::AuthService& auth_;
    std::mutex&               authMutex_;
};

} // namespace osp::server


