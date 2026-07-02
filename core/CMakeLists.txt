add_library(neothemis_core
    src/JudgeCore.cpp
)

target_include_directories(neothemis_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
