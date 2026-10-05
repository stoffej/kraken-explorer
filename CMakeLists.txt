cmake_minimum_required(VERSION 3.24)

project(kraken_explorer VERSION 0.0.3 LANGUAGES C CXX)

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
