#pragma once

#include "common/types.hpp"

#include <string>

namespace osp::domain
{

struct Paper
{
    PaperId id{};
    UserId author{};
    std::string title;
};

} // namespace osp::domain


