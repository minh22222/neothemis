add_executable(neothemis-cli
    main.cpp
)

target_link_libraries(neothemis-cli PRIVATE neothemis_core)
set_target_properties(neothemis-cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
