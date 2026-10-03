# Every ui/*.cpp belongs to this library, so new windows only have to add their file.
file(GLOB ui_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_library(kraken_ui STATIC ${ui_sources})
target_include_directories(kraken_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..) # #include "ui/theme.h"
target_link_libraries(kraken_ui PUBLIC imgui implot) # ui/graph.cpp
target_link_libraries(kraken_ui PUBLIC kraken_decoders) # ui/trace_window.cpp: UDS/J1939 tabs
# ui/*.cpp that draw from App& (main_menu.cpp, windows) need everything app.h includes.
target_link_libraries(kraken_ui PUBLIC kraken_core kraken_tasks kraken_setup)
target_link_libraries(kraken_ui PUBLIC kraken_drivers) # ui/setup_dialog.cpp: ifaces_find, ifaces_default_setup

# Fonts are embedded with imgui's binary_to_compressed_c (stb-compressed, u32 arrays).
add_executable(binary_to_compressed_c ${imgui_SOURCE_DIR}/misc/fonts/binary_to_compressed_c.cpp)
set(font_dir ${CMAKE_SOURCE_DIR}/src/assets/fonts)
set(font_out ${CMAKE_CURRENT_BINARY_DIR}/fonts)
foreach(font NotoSans NotoSansMono)
    add_custom_command(OUTPUT ${font_out}/${font}.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${font_out}
        COMMAND binary_to_compressed_c ${font_dir}/${font}-Regular.ttf ${font} > ${font_out}/${font}.h
        DEPENDS binary_to_compressed_c ${font_dir}/${font}-Regular.ttf
        VERBATIM)
    list(APPEND font_headers ${font_out}/${font}.h)
endforeach()
target_sources(kraken_ui PRIVATE ${font_headers})
target_include_directories(kraken_ui PRIVATE ${font_out})

# Icons (ui/icons.cpp): SVGs embedded as raw strings, rasterised with nanosvg at start-up.
# Order must match enum class Icon in ui/icons.h.
set(icon_dir ${CMAKE_SOURCE_DIR}/src/assets)
set(ICON_SVGS # Lucide (ISC), same order as enum class Icon in ui/icons.h
    icons/folder-open.svg icons/save-all.svg icons/save.svg icons/eraser.svg
    icons/play.svg icons/square.svg icons/settings-2.svg icons/refresh-cw.svg
    icons/chart-line.svg icons/filter.svg icons/folder.svg icons/file.svg
    icons/arrow-up.svg icons/house.svg icons/circle-dot.svg icons/history.svg
    icons/database.svg icons/arrow-right-left.svg icons/pause.svg icons/step-forward.svg)
set(icon_inc "const char* const icon_svgs[] = {\n")
foreach(svg IN LISTS ICON_SVGS)
    file(READ ${icon_dir}/${svg} svg_text)
    string(APPEND icon_inc "R\"svg(${svg_text})svg\",\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${icon_dir}/${svg})
endforeach()
string(APPEND icon_inc "};\n")
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/icons/icons_data.inc CONTENT "${icon_inc}" @ONLY)
# App icon (ui/theme.cpp theme_app_icon): full colour, so not part of the tinted atlas above.
file(READ ${icon_dir}/kraken.svg svg_text)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/icons/kraken_svg.inc
     CONTENT "const char* const kraken_svg = R\"svg(${svg_text})svg\";\n" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${icon_dir}/kraken.svg)
target_include_directories(kraken_ui PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/icons)
target_link_libraries(kraken_ui PRIVATE nanosvg)
if(WIN32)
    target_link_libraries(kraken_ui PRIVATE dwmapi) # ui/theme.cpp: dark title bar
endif()
target_link_libraries(kraken_ui PRIVATE ZLIB::ZLIB) # ui/replay.cpp: BLF log containers
target_link_libraries(kraken_ui PUBLIC kraken_python) # ui/script_window.cpp: python_run & co
