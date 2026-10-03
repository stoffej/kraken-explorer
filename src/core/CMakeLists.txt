# core/serial: serial port wrapper (SLCAN, GrIP): termios, Win32 COM ports on Windows.
add_library(kraken_serial STATIC $<IF:$<BOOL:${WIN32}>,serial_win32.cpp,serial.cpp>)
target_include_directories(kraken_serial PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..) # #include "core/serial.h"
if(WIN32)
    target_link_libraries(kraken_serial PRIVATE setupapi) # COM port list
endif()

# core: BusMessage POD + log ring buffer. Later core modules add to it with target_sources.
add_library(kraken_core STATIC bus_message.cpp log.cpp)
target_include_directories(kraken_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..) # #include "core/bus_message.h"
target_link_libraries(kraken_core PUBLIC Threads::Threads)

# core/platform: files, mappings, memory hints and directories, one implementation per OS.
target_sources(kraken_core PRIVATE $<IF:$<BOOL:${WIN32}>,platform_win32.cpp,platform_posix.cpp>)
if(WIN32)
    target_link_libraries(kraken_core PRIVATE winmm) # timeBeginPeriod
endif()

# core/tasks: main-thread queue. std::move_only_function needs C++23, so only this
# target and its users are raised from the project-wide C++20.
add_library(kraken_tasks STATIC tasks.cpp)
target_include_directories(kraken_tasks PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..) # #include "core/tasks.h"
target_compile_features(kraken_tasks PUBLIC cxx_std_23)
target_link_libraries(kraken_tasks PUBLIC Threads::Threads)

# core trace formats: text lines, pcapng blocks, whole export files (std::ostream).
target_sources(kraken_core PRIVATE trace_line_format.cpp pcapng.cpp trace_file_writer.cpp)

# core/trace: chunked trace store + per-interface RX inbox (main-thread drain).
target_sources(kraken_core PRIVATE trace.cpp png.cpp)
target_link_libraries(kraken_core PRIVATE ZLIB::ZLIB) # png.cpp

# core/trace_recorder: streaming recording to ASC/candump/pcapng while armed.
target_sources(kraken_core PRIVATE trace_recorder.cpp)

# core/setup: networks/interfaces/databases + <setup> XML (pugixml). Separate target
# because it needs kraken_db, which itself links kraken_core.
add_library(kraken_setup STATIC setup.cpp)
target_link_libraries(kraken_setup PUBLIC kraken_db pugixml::pugixml)

# core/python_engine: embedded Python (pybind11) scripting, the `kraken` module. Own
# target: it includes app.h (through the include root) and links the Python runtime.
add_library(kraken_python STATIC python_engine.cpp)
target_link_libraries(kraken_python PUBLIC kraken_tasks kraken_drivers PRIVATE pybind11::embed)
