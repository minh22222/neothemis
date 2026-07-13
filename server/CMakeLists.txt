if (NOT NEOTHEMIS_BUILD_SERVER)
    return()
endif()

find_package(Qt6 COMPONENTS Core Network Sql QUIET)
if (NOT Qt6_FOUND)
    message(STATUS "Qt6 Core/Network/Sql not found; neothemis-server target disabled")
    return()
endif()

add_library(neothemis_server_http STATIC
    http/src/Http.cpp
)
target_include_directories(neothemis_server_http PUBLIC http/include)
target_link_libraries(neothemis_server_http PUBLIC Qt6::Core Qt6::Network)

if (BUILD_TESTING)
    add_executable(neothemis-server-http-tests
        http/tests/HttpTests.cpp
    )
    target_link_libraries(neothemis-server-http-tests PRIVATE neothemis_server_http)
    add_test(NAME server.http COMMAND neothemis-server-http-tests)
endif()

add_executable(neothemis-server main.cpp)

qt_add_resources(neothemis-server neothemis_server_resources
    PREFIX "/"
    BASE ${CMAKE_SOURCE_DIR}
    FILES ${CMAKE_SOURCE_DIR}/materials/logo.png
)

target_link_libraries(neothemis-server PRIVATE
    neothemis_core
    neothemis_server_database
    neothemis_server_http
    Qt6::Core
    Qt6::Network
)
set_target_properties(neothemis-server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
