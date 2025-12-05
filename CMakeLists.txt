cmake_minimum_required(VERSION 3.14)

project(OsPeerReviewSys LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OSP_ENABLE_WARNINGS "Enable compiler warnings" ON)

if (OSP_ENABLE_WARNINGS)
    if (MSVC)
        add_compile_options(/W4)
    else ()
        add_compile_options(-Wall -Wextra -Wpedantic)
    endif ()
endif ()

add_subdirectory(src)


