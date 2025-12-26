#include "server_app.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
std::size_t parseSizeOrDefault(const char* s, std::size_t def)
{
    if (!s || *s == '\0')
    {
        return def;
    }
    try
    {
        return static_cast<std::size_t>(std::stoull(std::string{s}));
    }
    catch (...)
    {
        return def;
    }
}

std::uint16_t parsePortOrDefault(const char* s, std::uint16_t def)
{
    if (!s || *s == '\0')
    {
        return def;
    }
    try
    {
        auto v = std::stoul(std::string{s});
        if (v > 65535) return def;
        return static_cast<std::uint16_t>(v);
    }
    catch (...)
    {
        return def;
    }
}
}

int main(int argc, char** argv)
{
    // 用法：osproj_server [port] [cacheCapacity]
    // 也可通过环境变量 OSP_CACHE_CAPACITY 覆盖默认缓存容量。
    std::uint16_t port = 5555;
    std::size_t   cacheCapacity = parseSizeOrDefault(std::getenv("OSP_CACHE_CAPACITY"), 64);

    if (argc >= 2)
    {
        port = parsePortOrDefault(argv[1], port);
    }
    if (argc >= 3)
    {
        cacheCapacity = parseSizeOrDefault(argv[2], cacheCapacity);
    }

    osp::server::ServerApp app(port, cacheCapacity);
    app.run();
    return 0;
}


