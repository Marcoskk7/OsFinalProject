#pragma once

#include "common/protocol.hpp"
#include "server/filesystem/vfs.hpp"

#include <mutex>
#include <optional>

namespace osp::server
{

class FsService
{
public:
    FsService(osp::fs::Vfs& vfs, std::mutex& vfsMutex)
        : vfs_(vfs)
        , vfsMutex_(vfsMutex)
    {
    }

    // 返回 std::nullopt 表示不是 FS 命令
    std::optional<osp::protocol::Message> tryHandle(const osp::protocol::Command& cmd);

private:
    osp::fs::Vfs& vfs_;
    std::mutex&   vfsMutex_;
};

} // namespace osp::server


