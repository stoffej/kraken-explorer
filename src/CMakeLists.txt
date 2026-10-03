foreach(m core db decoders drivers ui)
    add_subdirectory(${m})
endforeach()

# app.cpp (per-frame flow, measurement start/stop) and core/rest_api.cpp (HTTP control, calls it):
# a library so the tests reach them too.
add_library(kraken_app STATIC app.cpp core/rest_api.cpp)
target_link_libraries(kraken_app PUBLIC kraken_ui kraken_python)
if(WIN32)
    target_link_libraries(kraken_app PUBLIC ws2_32) # core/rest_api.cpp
endif()

add_executable(kraken-explorer main.cpp)
target_link_libraries(kraken-explorer PRIVATE imgui kraken_app)
if(WIN32)
    # The manifest (UTF-8 code page) of every executable: the app and the tests.
    add_library(kraken_manifest INTERFACE)
    target_sources(kraken_manifest INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/windows/kraken-explorer.rc)
    if(MINGW)
        # GCC adds its own default-manifest.o to every link. Next to ours ld reports "multiple
        # non-default manifests" and a Release link crashes (binutils 2.47), leaving a truncated
        # exe. An empty object of that name, found first through -B, leaves ours as the only one.
        set(manifest_dir ${CMAKE_CURRENT_BINARY_DIR}/manifest)
        file(WRITE ${manifest_dir}/empty.c "")
        add_custom_command(OUTPUT ${manifest_dir}/default-manifest.o
            COMMAND ${CMAKE_C_COMPILER} -c ${manifest_dir}/empty.c -o ${manifest_dir}/default-manifest.o)
        add_custom_target(kraken_default_manifest DEPENDS ${manifest_dir}/default-manifest.o)
        add_dependencies(kraken_manifest kraken_default_manifest)
        target_link_options(kraken_manifest INTERFACE -B${manifest_dir}/)
    endif()
    target_link_libraries(kraken-explorer PRIVATE kraken_manifest)
    # The example scripts next to the exe, as in the zip: ui/script_window.cpp looks there.
    add_custom_command(TARGET kraken-explorer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/examples $<TARGET_FILE_DIR:kraken-explorer>/examples)
endif()
