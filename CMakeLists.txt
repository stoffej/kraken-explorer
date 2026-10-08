cmake_minimum_required(VERSION 3.24)

# The version is the newest release tag (x.y.z) reachable from HEAD; CI checks out with full
# history so it finds it. Without git or tags: 0.0.0. -DKRAKEN_VERSION=x.y.z overrides.
# ponytail: read at configure time only, re-run cmake after tagging to pick up a new tag.
if(NOT KRAKEN_VERSION)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0 --match "[0-9]*.[0-9]*.[0-9]*"
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                        OUTPUT_VARIABLE KRAKEN_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if(NOT KRAKEN_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        set(KRAKEN_VERSION 0.0.0)
    endif()
endif()

project(kraken_explorer VERSION ${KRAKEN_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(KRAKEN_SANITIZE "Build everything with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(KRAKEN_TESTS "Build the doctest unit tests" ON)

# Applied before the dependencies are fetched so third-party code is instrumented too.
if(KRAKEN_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

add_compile_definitions(VERSION_STRING="${PROJECT_VERSION}")
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00) # Windows 10 API level (PrefetchVirtualMemory, WSAPoll, inet_pton)
    if(MINGW)
        add_link_options(-static) # libstdc++, libgcc and winpthread into the exe: only python3xx.dll ships beside it
    endif()
endif()

include(cmake/deps.cmake)

# Each module lives in its own src/<module>/CMakeLists.txt (see src/CMakeLists.txt).
add_subdirectory(src)

if(KRAKEN_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
