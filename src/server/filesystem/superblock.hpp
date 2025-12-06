#pragma once

#include <cstdint>

namespace osp::fs
{

// 简化版 SuperBlock，描述磁盘上自定义文件系统的整体布局。
// 磁盘（backing file）按「块」划分：
// [0]             : superblock
// [inodeTableStart .. inodeTableStart + inodeTableBlocks - 1] : inode table
// [freeBitmapStart .. freeBitmapStart + freeBitmapBlocks - 1] : free data-block bitmap
// [dataBlockStart .. totalBlocks - 1]                         : data blocks
//
// 具体块数可在挂载/格式化时固定为一组常量以满足课程要求。
struct SuperBlock
{
    std::uint32_t magic{0x20251205};     // 文件系统魔数，用于校验是否为本 FS
    std::uint32_t blockSize{4096};       // 逻辑块大小（字节）
    std::uint32_t totalBlocks{0};        // 整个 backing file 被划分为多少个块

    // inode 表信息
    std::uint32_t inodeTableStart{0};    // inode 表起始块号
    std::uint32_t inodeTableBlocks{0};   // inode 表占用的块数
    std::uint32_t inodeCount{0};         // inode 总数量

    // 空闲数据块位图信息（只管理 dataBlockStart 之后的数据块）
    std::uint32_t freeBitmapStart{0};    // 空闲块位图起始块号
    std::uint32_t freeBitmapBlocks{0};   // 空闲块位图占用的块数

    // 数据块区域
    std::uint32_t dataBlockStart{0};     // 第一个数据块的块号
    std::uint32_t dataBlockCount{0};     // 数据块数量

    // 根目录 inode 号（后续实现目录时可使用）
    std::uint32_t rootInodeId{0};
};

} // namespace osp::fs


