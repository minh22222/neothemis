find_package(Qt6 COMPONENTS Core Sql QUIET)
if (NOT Qt6_FOUND)
    message(STATUS "Qt6 Core/Sql not found; shared server database target disabled")
    return()
endif()

add_library(neothemis_server_database STATIC
    src/Database.cpp
)
target_include_directories(neothemis_server_database PUBLIC include)
target_link_libraries(neothemis_server_database PUBLIC Qt6::Core Qt6::Sql)

if (BUILD_TESTING)
    add_executable(neothemis-server-database-tests
        tests/DatabaseTests.cpp
    )
    target_link_libraries(neothemis-server-database-tests PRIVATE
        neothemis_server_database
    )
    add_test(NAME server.database COMMAND neothemis-server-database-tests)
    set_tests_properties(server.database PROPERTIES TIMEOUT 90)
endif()
