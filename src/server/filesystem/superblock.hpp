#pragma once

#include <cstdint>

namespace osp::fs
{

struct SuperBlock
{
    std::uint32_t magic{0x20251205}; // 任意魔数
    std::uint32_t blockSize{4096};
    std::uint32_t totalBlocks{0};
    std::uint32_t inodeTableStart{0};
    std::uint32_t dataBlockStart{0};
};

} // namespace osp::fs


