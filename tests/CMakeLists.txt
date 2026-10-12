include(Catch)

# Test binaries live under tests/... while Conan shared libraries are copied to
# build/<type>/lib. These parent-relative RPATHs let Catch2 discovery execute them.
# $ORIGIN entries serve Linux (ld.so); @loader_path entries serve macOS (dyld) —
# each loader ignores the other's spelling, so both are listed unconditionally.
function(otterstax_discover_tests target)
    set_target_properties(
        ${target} PROPERTIES
        INSTALL_RPATH "$ORIGIN/lib;$ORIGIN;$ORIGIN/../lib;$ORIGIN/../../lib;$ORIGIN/../../../lib;$ORIGIN/../../../../lib;$ORIGIN/../../../../../lib;@loader_path/lib;@loader_path;@loader_path/../lib;@loader_path/../../lib;@loader_path/../../../lib;@loader_path/../../../../lib;@loader_path/../../../../../lib"
    )
    catch_discover_tests(${target})
endfunction()

add_subdirectory(mysql-front)
add_subdirectory(unit)
add_subdirectory(system)

add_executable(snowflake_transport_probe snowflake_transport_probe.cpp)
target_link_libraries(snowflake_transport_probe PRIVATE connectors)
set_target_properties(snowflake_transport_probe PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib;@loader_path/../lib")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME snowflake_odbc_transport
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_snowflake_transport.py
        --probe $<TARGET_FILE:snowflake_transport_probe>
        --driver ${CMAKE_BINARY_DIR}/lib/libsfodbc.so)
    set_tests_properties(snowflake_odbc_transport PROPERTIES TIMEOUT 90)
endif()
