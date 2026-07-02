if (NOT NEOTHEMIS_BUILD_GUI)
    return()
endif()

find_package(Qt6 COMPONENTS Widgets QUIET)
if (NOT Qt6_FOUND)
    message(STATUS "Qt6 Widgets not found; neothemis-gui target disabled")
    return()
endif()

add_executable(neothemis-gui
    main.cpp
)

qt_add_resources(neothemis-gui neothemis_gui_resources
    PREFIX "/"
    BASE ${CMAKE_SOURCE_DIR}
    FILES ${CMAKE_SOURCE_DIR}/materials/logo.png
)

target_link_libraries(neothemis-gui PRIVATE neothemis_core Qt6::Widgets)
target_compile_definitions(neothemis-gui PRIVATE QT_NO_KEYWORDS)
set_target_properties(neothemis-gui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
