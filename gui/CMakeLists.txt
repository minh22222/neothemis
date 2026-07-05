if (NOT NEOTHEMIS_BUILD_GUI)
    return()
endif()

find_package(Qt6 COMPONENTS Widgets Sql QUIET)
if (NOT Qt6_FOUND)
    message(STATUS "Qt6 Widgets/Sql not found; neothemis-gui target disabled")
    return()
endif()
find_package(ZLIB QUIET)
find_package(Qt6ZlibPrivate QUIET)

add_executable(neothemis-gui
    main.cpp
    MainWindow.cpp
    MainWindow.hpp
    FileFormats.cpp
    FileFormats.hpp
    FileAssociation.cpp
    FileAssociation.hpp
    GuiSupport.cpp
    GuiSupport.hpp
    Theme.cpp
    Theme.hpp
    ThemeBackground.cpp
    ThemeBackground.hpp
    Translations.cpp
    Translations.hpp
    WindowResizeHandles.cpp
    WindowResizeHandles.hpp
)

if (WIN32)
    target_sources(neothemis-gui PRIVATE ${CMAKE_SOURCE_DIR}/materials/neothemis.rc)
    target_link_libraries(neothemis-gui PRIVATE shell32)
endif()

qt_add_resources(neothemis-gui neothemis_gui_resources
    PREFIX "/"
    BASE ${CMAKE_SOURCE_DIR}
    FILES ${CMAKE_SOURCE_DIR}/materials/logo.png
)

target_link_libraries(neothemis-gui PRIVATE neothemis_core Qt6::Widgets Qt6::Sql)
if (TARGET Qt6::ZlibPrivate)
    target_link_libraries(neothemis-gui PRIVATE Qt6::ZlibPrivate)
    target_compile_definitions(neothemis-gui PRIVATE NEOTHEMIS_HAS_ZLIB=1)
elseif (ZLIB_FOUND)
    target_link_libraries(neothemis-gui PRIVATE ZLIB::ZLIB)
    target_compile_definitions(neothemis-gui PRIVATE NEOTHEMIS_HAS_ZLIB=1)
endif()
target_compile_definitions(neothemis-gui PRIVATE QT_NO_KEYWORDS)
set_target_properties(neothemis-gui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    WIN32_EXECUTABLE TRUE
)
