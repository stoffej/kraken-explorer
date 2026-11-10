# decoders: UDS (ISO-TP, LIN diagnostics), J1939 and NMEA 2000 protocol decoders.
add_library(kraken_decoders STATIC uds.cpp uds_client.cpp j1939.cpp n2k.cpp protocol.cpp)
target_include_directories(kraken_decoders PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..) # #include "decoders/decoders.h"
target_link_libraries(kraken_decoders PUBLIC kraken_core)
