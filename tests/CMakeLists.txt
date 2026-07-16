add_executable(neothemis-core-tests
    core_tests.cpp
)
target_link_libraries(neothemis-core-tests PRIVATE neothemis_core)
target_compile_definitions(neothemis-core-tests PRIVATE
    NEOTHEMIS_TEST_COMPILER="${CMAKE_CXX_COMPILER}"
)

if (WIN32)
    add_library(neothemis-windows-fixture-runtime SHARED
        windows_fixture_runtime.cpp
    )
    add_executable(neothemis-windows-fixture-submission
        windows_fixture_submission.cpp
    )
    target_link_libraries(neothemis-windows-fixture-submission PRIVATE
        neothemis-windows-fixture-runtime
    )
    add_executable(neothemis-windows-fake-compiler
        windows_fake_compiler.cpp
    )
    target_link_libraries(neothemis-windows-fake-compiler PRIVATE shell32)
    if (MINGW)
        target_link_options(neothemis-windows-fixture-runtime PRIVATE -static)
        target_link_options(neothemis-windows-fixture-submission PRIVATE -static)
        target_link_options(neothemis-windows-fake-compiler PRIVATE -static)
    endif()
    add_dependencies(neothemis-core-tests
        neothemis-windows-fixture-runtime
        neothemis-windows-fixture-submission
        neothemis-windows-fake-compiler
    )
    target_compile_definitions(neothemis-core-tests PRIVATE
        NEOTHEMIS_WINDOWS_FIXTURE_RUNTIME="$<TARGET_FILE:neothemis-windows-fixture-runtime>"
        NEOTHEMIS_WINDOWS_FIXTURE_SUBMISSION="$<TARGET_FILE:neothemis-windows-fixture-submission>"
        NEOTHEMIS_WINDOWS_FAKE_COMPILER="$<TARGET_FILE:neothemis-windows-fake-compiler>"
    )
endif()

add_test(NAME core.regressions COMMAND neothemis-core-tests)
set_tests_properties(core.regressions PROPERTIES TIMEOUT 90)

if (TARGET neothemis-server)
    find_package(Qt6 COMPONENTS Core Network Sql REQUIRED)
    add_executable(neothemis-server-integration-tests
        server_integration_tests.cpp
    )
    target_link_libraries(neothemis-server-integration-tests PRIVATE
        neothemis_server_database
        Qt6::Core Qt6::Network Qt6::Sql
    )
    target_compile_definitions(neothemis-server-integration-tests PRIVATE
        NEOTHEMIS_TEST_COMPILER="${CMAKE_CXX_COMPILER}"
    )
    add_test(NAME server.integration
        COMMAND neothemis-server-integration-tests $<TARGET_FILE:neothemis-server>
    )
    set_tests_properties(server.integration PROPERTIES TIMEOUT 90)
endif()
