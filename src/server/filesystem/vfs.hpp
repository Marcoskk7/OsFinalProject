#pragma once

#include "block_cache.hpp"
#include "inode.hpp"
#include "superblock.hpp"

#include <optional>
#include <string>

namespace osp::fs
{

// 非完整实现的虚拟文件系统接口，满足课程要求的架构预留
class Vfs
{
public:
    explicit Vfs(std::size_t cacheCapacity)
        : cache_(cacheCapacity)
    {
    }

    bool mount(const std::string& backingFile);

    [[nodiscard]] const SuperBlock& superBlock() const noexcept { return sb_; }

    // 目录 & 文件操作的占位接口
    std::optional<Inode> createFile(const std::string& path);
    bool removeFile(const std::string& path);

private:
    SuperBlock sb_{};
    BlockCache cache_;
    std::string backingFile_;
};

} // namespace osp::fs


