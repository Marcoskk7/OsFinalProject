#pragma once

#include <cstdint>

namespace osp::fs
{

struct Inode
{
    std::uint32_t id{};
    bool isDirectory{false};
    std::uint32_t size{0};
    std::uint32_t firstBlock{0};
};

} // namespace osp::fs


