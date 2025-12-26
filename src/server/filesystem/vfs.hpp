#pragma once

#include "block_cache.hpp"
#include "inode.hpp"
#include "superblock.hpp"

#include <cstddef>
#include <functional>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace osp::fs
{

// 简化版虚拟文件系统，负责：
// - 维护 superblock / inode 表 / 数据块区域的磁盘布局
// - 通过 BlockCache 进行块级读写缓存
class Vfs
{
public:
    explicit Vfs(std::size_t cacheCapacity)
        : cache_(cacheCapacity)
    {
    }

    // 挂载或初始化文件系统；如果 backingFile 不存在或不是合法的本项目文件系统，则自动格式化。
    bool mount(const std::string& backingFile);

    // 刷新底层文件（用于 BACKUP/RESTORE 前确保落盘）
    bool sync();

    // 重新挂载（会关闭并重开 backingFile_，并重置 BlockCache）
    // beforeOpen: 在关闭旧文件后、重新打开前执行（可用于外部覆盖 backingFile_ 内容，例如 RESTORE）
    bool remount(const std::function<bool(const std::string& backingFile)>& beforeOpen = {});

    [[nodiscard]] const SuperBlock& superBlock() const noexcept { return sb_; }
    [[nodiscard]] BlockCache::Stats cacheStats() const noexcept { return cache_.stats(); }
    [[nodiscard]] std::size_t cacheCapacity() const noexcept { return cache_.capacity(); }
    [[nodiscard]] std::size_t cacheSize() const noexcept { return cache_.size(); }

    // ------------ 高层文件/目录接口（带路径解析） ------------

    // 创建目录（不自动创建多级父目录，要求父目录已存在）
    bool createDirectory(const std::string& path);

    // 创建普通文件（如果已存在则返回该文件 inode），父目录必须已存在
    std::optional<Inode> createFile(const std::string& path);

    // 把一个字符串整体写入指定路径的文件（不存在则创建，存在则覆盖）
    bool writeFile(const std::string& path, const std::string& data);

    // 从指定路径读取整个文件内容为字符串
    std::optional<std::string> readFile(const std::string& path);

    // 删除普通文件（不支持递归删目录）
    bool removeFile(const std::string& path);

    // 删除空目录（不允许删除根目录，也不做递归删除）
    bool removeDirectory(const std::string& path);

    // 列出目录项，返回简单的字符串列表（每行一个名字），失败返回 std::nullopt
    std::optional<std::string> listDirectory(const std::string& path);

private:
    // 目录项结构：只支持 60 字节以内的 UTF-8 名称
    struct DirEntry
    {
        std::uint32_t inodeId{}; // 0 表示空槽
        char name[60]{};         // 以 '\0' 结尾的字符串
    };

    // --- 低层工具函数：块读写 & inode/位图管理 ---
    bool loadSuperBlock();
    bool flushSuperBlock();
    bool formatNewFileSystem();

    std::vector<std::byte> readBlock(std::uint32_t blockId);
    bool writeBlock(std::uint32_t blockId, const std::vector<std::byte>& data);

    bool loadInode(std::uint32_t id, Inode& out);
    bool storeInode(const Inode& ino);

    bool allocDataBlock(std::uint32_t& outBlockId);
    bool freeDataBlock(std::uint32_t blockId);

    bool findFreeInode(std::uint32_t& outInodeId);

    // --- 路径解析与目录操作 ---

    // 把 "/a/b/c" 切成 {"a","b","c"}，返回是否成功
    bool splitPath(const std::string& path, std::vector<std::string>& components) const;

    // 解析绝对路径，返回末尾组件对应的 inodeId（不创建）
    bool resolvePath(const std::string& path, std::uint32_t& outInodeId);

    // 解析父目录路径，得到父目录 inodeId 和最后一段名字（用于创建）
    bool resolveParentDirectory(const std::string& path,
                                std::uint32_t& outParentInodeId,
                                std::string& outName);

    // 读取目录 inode（仅使用 directBlocks[0]，单块目录）
    bool readDirectory(const Inode& dirInode, std::vector<DirEntry>& entries);

    // 写回目录内容（仅使用 directBlocks[0]，不支持超过一个块的超大目录）
    bool writeDirectory(Inode& dirInode, const std::vector<DirEntry>& entries);

private:
    SuperBlock sb_{};
    BlockCache cache_;
    std::string backingFile_;
    std::fstream file_;
};

} // namespace osp::fs


