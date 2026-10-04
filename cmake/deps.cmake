# Third-party dependencies. FetchContent ones are pinned to a tag or commit.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# -DKRAKEN_DEPS_DIR=<dir>: <dir>/<name>.tar.gz is used instead of the download when it exists
# (offline builds; the CMake bundled with WinLibs has no CA certificates for https).
function(kraken_fetch name url)
    file(TO_CMAKE_PATH "${KRAKEN_DEPS_DIR}" deps_dir) # C:\deps as typed on Windows: FetchContent chokes on the backslashes
    if(KRAKEN_DEPS_DIR AND EXISTS ${deps_dir}/${name}.tar.gz)
        set(url ${deps_dir}/${name}.tar.gz)
    endif()
    FetchContent_Declare(${name} URL ${url} DOWNLOAD_EXTRACT_TIMESTAMP ON ${ARGN})
endfunction()

kraken_fetch(imgui     https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b-docking.tar.gz)
kraken_fetch(implot    https://github.com/epezent/implot/archive/refs/tags/v1.0.tar.gz)
kraken_fetch(glfw      https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz)
kraken_fetch(pugixml   https://github.com/zeux/pugixml/archive/refs/tags/v1.15.tar.gz)
kraken_fetch(doctest   https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz)
kraken_fetch(nanosvg   https://github.com/memononen/nanosvg/archive/239e102ec2c691f2902e20ace2ed36ee4a35cfe6.tar.gz)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glfw pugixml doctest)
FetchContent_Populate(imgui)    # no upstream CMake
FetchContent_Populate(implot)
FetchContent_Populate(nanosvg)

find_package(OpenGL REQUIRED)

# imgui core + glfw/opengl3 backends
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
# No IMGUI_DISABLE_OBSOLETE_FUNCTIONS: implot v1.0 still calls the deprecated AddPolyline overload.
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)
# 32-bit ImWchar: codepoints above U+FFFF (the squid glyph in ui/theme, emoji typed into text fields).
target_compile_definitions(imgui PUBLIC IMGUI_USE_WCHAR32)

add_library(implot STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp)
target_include_directories(implot SYSTEM PUBLIC ${implot_SOURCE_DIR})
# implot_items.cpp instantiates every plotter for every type: with ASan/UBSan at -O2 -g it
# takes >10 min to compile. Plot templates are not ours to sanitize, so build it plainly (~1 min).
set_source_files_properties(${implot_SOURCE_DIR}/implot_items.cpp PROPERTIES COMPILE_OPTIONS "-fno-sanitize=all;-O1;-g0")
target_link_libraries(implot PUBLIC imgui)

# Header-only
add_library(nanosvg INTERFACE)
target_include_directories(nanosvg SYSTEM INTERFACE ${nanosvg_SOURCE_DIR}/src)

find_package(Threads REQUIRED)
if(WIN32)
    # No pkg-config world on Windows: zlib, pybind11 and libusb are fetched and built here, Python
    # is the python.org install. SocketCAN (libnl) does not exist.
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    kraken_fetch(zlib     https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz)
    kraken_fetch(pybind11 https://github.com/pybind/pybind11/archive/refs/tags/v2.13.6.tar.gz)
    kraken_fetch(libusb   https://github.com/libusb/libusb-cmake/archive/refs/tags/v1.0.27-1.tar.gz)
    set(LIBUSB_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zlib libusb)
    target_include_directories(zlibstatic INTERFACE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
    set_target_properties(zlib PROPERTIES EXCLUDE_FROM_ALL ON) # the DLL: only zlibstatic is linked
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
    add_library(PkgConfig::LIBUSB ALIAS usb-1.0)
    set(PYBIND11_FINDPYTHON ON)
    find_package(Python 3.9 COMPONENTS Interpreter Development.Embed REQUIRED)
    FetchContent_MakeAvailable(pybind11)
else()
    # System dependencies (pkg-config), exposed as PkgConfig::<NAME> targets
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
    pkg_check_modules(LIBNL REQUIRED IMPORTED_TARGET libnl-3.0 libnl-route-3.0)
    pkg_check_modules(PYTHON REQUIRED IMPORTED_TARGET python3-embed)
    find_package(pybind11 CONFIG REQUIRED)
    find_package(ZLIB REQUIRED) # core/png.cpp (deflate), ui/replay.cpp (BLF containers)
endif()
