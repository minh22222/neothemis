add_library(neothemis_core
    src/Config.cpp
    src/Csv.cpp
    src/JudgeCore.cpp
    src/ContestArchive.cpp
    src/Spreadsheet.cpp
    src/Sandbox.cpp
)

target_include_directories(neothemis_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(ZLIB QUIET)
find_package(Qt6 COMPONENTS Core QUIET)
find_package(Qt6ZlibPrivate QUIET)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(SECCOMP QUIET IMPORTED_TARGET libseccomp)
    endif()
    if (TARGET PkgConfig::SECCOMP)
        target_link_libraries(neothemis_core PRIVATE PkgConfig::SECCOMP)
        target_compile_definitions(neothemis_core PRIVATE NEOTHEMIS_HAS_SECCOMP=1)
    endif()
endif()

if (ZLIB_FOUND)
    target_link_libraries(neothemis_core PUBLIC ZLIB::ZLIB)
    target_compile_definitions(neothemis_core PUBLIC NEOTHEMIS_HAS_ZLIB=1)
elseif (TARGET Qt6::ZlibPrivate AND TARGET Qt6::Core)
    target_link_libraries(neothemis_core PUBLIC Qt6::Core Qt6::ZlibPrivate)
    target_compile_definitions(neothemis_core PUBLIC NEOTHEMIS_HAS_ZLIB=1)
endif()
