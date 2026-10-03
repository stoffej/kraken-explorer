// Thin serial port wrapper: termios (serial.cpp), Win32 COM ports (serial_win32.cpp).
// Always 8N1, no flow control, raw mode - the only setup SLCAN and GrIP use.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SerialPort
{
    intptr_t fd = -1; // file descriptor; a HANDLE on Windows
    std::string error; // last error message, set when a call fails
};

struct SerialPortInfo
{
    std::string name;   // "/dev/ttyACM0" (Windows: "COM7"), passed as is to serial_open()
    uint16_t vid = 0;   // 0 when not a USB device
    uint16_t pid = 0;
    std::string serial; // USB serial number, empty when unknown
};

[[nodiscard]] bool serial_open(SerialPort &port, const std::string &name, int baud);
void serial_close(SerialPort &port) noexcept;
[[nodiscard]] bool serial_is_open(const SerialPort &port) noexcept;

// Blocks until everything is written. Returns false on error.
bool serial_write(SerialPort &port, const void *data, size_t size);

// Waits up to `timeout` for data, then reads what is available (at most `size`).
// Returns bytes read, 0 on timeout, -1 on error (e.g. device unplugged).
[[nodiscard]] long serial_read(SerialPort &port, void *data, size_t size, std::chrono::milliseconds timeout);

// Drops unread input and unsent output.
void serial_clear(SerialPort &port) noexcept;

[[nodiscard]] std::vector<SerialPortInfo> serial_list_ports();
