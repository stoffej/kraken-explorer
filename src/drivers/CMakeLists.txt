# drivers: DriverOps tables + Iface/listener threads. A new driver adds its .cpp with
# target_sources and its DriverOps to the list in driver.cpp.
add_library(kraken_drivers STATIC driver.cpp)
target_include_directories(kraken_drivers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..) # #include "drivers/driver.h"
target_link_libraries(kraken_drivers PUBLIC kraken_setup Threads::Threads)

target_sources(kraken_drivers PRIVATE slcan.cpp)
target_sources(kraken_drivers PRIVATE grip.cpp)
target_link_libraries(kraken_drivers PRIVATE kraken_tasks) # grip: GPIO/caps via core/tasks
target_link_libraries(kraken_drivers PRIVATE kraken_serial)

target_sources(kraken_drivers PRIVATE canblast.cpp)

target_sources(kraken_drivers PRIVATE socketcan_link.cpp) # `ip` command lines: portable
if(NOT WIN32)
    target_sources(kraken_drivers PRIVATE socketcan.cpp)
    target_link_libraries(kraken_drivers PRIVATE PkgConfig::LIBNL)
else()
    target_link_libraries(kraken_drivers PRIVATE ws2_32) # canblast.cpp
endif()

# Vendor interface of the 1d50:606f composite adapter: lin_usb (LindeAPI)
target_sources(kraken_drivers PRIVATE linde/linde.cpp usb_vendor/usb_vendor_libusb.cpp)
target_link_libraries(kraken_drivers PRIVATE PkgConfig::LIBUSB)

# Kvaser (linuxcan CANlib): the driver is always built; libcanlib is dlopen'ed at run time when
# installed, so the SDK (not redistributable) is needed neither to build nor to package.
target_sources(kraken_drivers PRIVATE kvaser.cpp)
target_link_libraries(kraken_drivers PRIVATE ${CMAKE_DL_LIBS})
