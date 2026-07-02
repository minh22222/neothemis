cmake_minimum_required(VERSION 3.16)

project(NeoThemis LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(neothemis_core
    core/src/JudgeCore.cpp
)

target_include_directories(neothemis_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/core/include
)

add_executable(neothemis-cli
    cli/main.cpp
)

target_link_libraries(neothemis-cli PRIVATE neothemis_core)

if (MSVC)
    target_compile_options(neothemis_core PRIVATE /W4)
    target_compile_options(neothemis-cli PRIVATE /W4)
else()
    target_compile_options(neothemis_core PRIVATE -Wall -Wextra -pedantic)
    target_compile_options(neothemis-cli PRIVATE -Wall -Wextra -pedantic)
endif()
