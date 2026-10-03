# One doctest binary per tests/<area>/<area>_test.cpp: target <area>_test, ctest name <area>,
# binary in build/tests/<area>/. Every test links the whole project (static libs only pull in
# what a test references), so a new area needs no CMake. util: pseudo terminals (serial, slcan, grip).
file(GLOB test_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*/*_test.cpp)
if(WIN32)
    set(test_libs ZLIB::ZLIB)
    set(linux_only drivers grip_driver serial slcan_driver) # SocketCAN (vcan) and pseudo terminals
else()
    set(test_libs ZLIB::ZLIB util)
endif()
foreach(src IN LISTS test_sources)
    cmake_path(GET src PARENT_PATH dir)
    cmake_path(GET dir FILENAME area)
    if(area IN_LIST linux_only)
        continue()
    endif()
    add_executable(${area}_test ${src})
    target_link_libraries(${area}_test PRIVATE doctest::doctest kraken_ui kraken_db kraken_serial kraken_app ${test_libs})
    target_include_directories(${area}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}) # #include "ui_test.h"
    set_target_properties(${area}_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${area})
    add_test(NAME ${area} COMMAND ${area}_test)
endforeach()

# Areas with more than the binary: compile definitions, data files, extra programs.
foreach(area format_compat replay setup sym_parser tx_generator)
    add_subdirectory(${area})
endforeach()
