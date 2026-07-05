if (NOT NEOTHEMIS_BUILD_SERVER)
    return()
endif()

find_package(Qt6 COMPONENTS Core Network Sql QUIET)
if (NOT Qt6_FOUND)
    message(STATUS "Qt6 Core/Network/Sql not found; neothemis-server target disabled")
    return()
endif()

add_executable(neothemis-server
    main.cpp
)

qt_add_resources(neothemis-server neothemis_server_resources
    PREFIX "/"
    BASE ${CMAKE_SOURCE_DIR}
    FILES ${CMAKE_SOURCE_DIR}/materials/logo.png
)

target_link_libraries(neothemis-server PRIVATE neothemis_core Qt6::Core Qt6::Network Qt6::Sql)
set_target_properties(neothemis-server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
