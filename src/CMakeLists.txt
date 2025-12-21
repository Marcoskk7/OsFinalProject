add_library(osproj_common INTERFACE)

target_include_directories(osproj_common
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

add_library(osproj_domain
    domain/user.hpp
    domain/user.cpp
    domain/roles.hpp
    domain/paper.hpp
    domain/review.hpp
    domain/permissions.hpp
    domain/permissions.cpp
    domain/auth.hpp
    domain/auth.cpp
)

target_link_libraries(osproj_domain
    PUBLIC
        osproj_common
)

add_library(osproj_fs
    server/filesystem/vfs.hpp
    server/filesystem/vfs.cpp
    server/filesystem/superblock.hpp
    server/filesystem/inode.hpp
    server/filesystem/block_cache.hpp
)

target_link_libraries(osproj_fs
    PUBLIC
        osproj_common
)

add_library(osproj_server_core
    server/server_app.hpp
    server/server_app.cpp
    server/net/tcp_server.hpp
    server/net/tcp_server.cpp
)

target_link_libraries(osproj_server_core
    PUBLIC
        osproj_common
        osproj_domain
        osproj_fs
)

add_executable(osproj_server
    server/main.cpp
)

target_link_libraries(osproj_server
    PRIVATE
        osproj_server_core
)

add_executable(osproj_client
    client/main.cpp
    client/cli.hpp
    client/cli.cpp
    client/net/tcp_client.hpp
    client/net/tcp_client.cpp
)

target_link_libraries(osproj_client
    PRIVATE
        osproj_common
)


