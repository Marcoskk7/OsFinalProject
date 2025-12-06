#pragma once

#include <cstdint>

namespace osp::fs
{

// 简化版 inode 结构：
// - 本实现仅使用少量直接块指针，不实现间接块，以便代码保持清晰。
// - 磁盘上的 inode 表就是该结构的顺序数组。
struct Inode
{
    std::uint32_t id{};              // inode 编号（在 inode 表中的索引）
    bool          isDirectory{false};
    std::uint32_t size{0};           // 文件大小（字节）

    // 直接数据块指针，支持小文件即可满足课程实验需求
    static constexpr std::size_t MaxDirectBlocks = 8;
    std::uint32_t directBlocks[MaxDirectBlocks]{};
};

} // namespace osp::fs


