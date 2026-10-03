#include "serial.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <initguid.h> // before the GUID headers: defines GUID_DEVINTERFACE_COMPORT here
#include <devguid.h>
#include <ntddser.h>
#include <setupapi.h>

// Win32 COM port backend of core/serial.h. The handle is overlapped so a write (main thread)
// does not wait behind the listener thread's blocking read, as it would on a synchronous handle.

static HANDLE handle_of(const SerialPort &port)
{
    return reinterpret_cast<HANDLE>(port.fd);
}

static std::string error_text(DWORD code = GetLastError())
{
    char buf[256] = {};
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, sizeof buf, nullptr);
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.'))
    {
        s.pop_back();
    }
    return s.empty() ? "error " + std::to_string(code) : s;
}

// Runs one overlapped ReadFile/WriteFile to completion. Bytes transferred, or -1 with `error` set.
// ponytail: an event per call, keep one per direction in SerialPort if a profile ever shows it
template <typename Io>
static long overlapped_io(SerialPort &port, Io &&io)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD n = 0;
    bool ok = ov.hEvent != nullptr && (io(&ov) || GetLastError() == ERROR_IO_PENDING);
    ok = ok && GetOverlappedResult(handle_of(port), &ov, &n, TRUE);
    if (!ok)
    {
        port.error = error_text();
    }
    if (ov.hEvent != nullptr)
    {
        CloseHandle(ov.hEvent);
    }
    return ok ? static_cast<long>(n) : -1;
}

bool serial_open(SerialPort &port, const std::string &name, int baud)
{
    serial_close(port);
    if (baud <= 0)
    {
        port.error = name + ": unsupported baud rate " + std::to_string(baud);
        return false;
    }
    // "\\.\COM10": the device namespace, needed above COM9 and harmless below.
    const std::string path = name.starts_with("\\\\") ? name : "\\\\.\\" + name;
    const HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        port.error = name + ": " + error_text();
        return false;
    }
    DCB dcb{};
    dcb.DCBlength = sizeof dcb;
    bool ok = GetCommState(h, &dcb) != 0;
    dcb.BaudRate = static_cast<DWORD>(baud);
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE; // as Linux asserts DTR/RTS on open (USB CDC adapters wait for DTR)
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    ok = ok && SetCommState(h, &dcb) != 0;
    // A read returns at once with what is there, or waits up to the constant for the first byte
    // (set per call in serial_read); writes do not time out.
    COMMTIMEOUTS to{.ReadIntervalTimeout = MAXDWORD, .ReadTotalTimeoutMultiplier = MAXDWORD, .ReadTotalTimeoutConstant = 1,
                    .WriteTotalTimeoutMultiplier = 0, .WriteTotalTimeoutConstant = 0};
    ok = ok && SetCommTimeouts(h, &to) != 0;
    if (!ok)
    {
        port.error = name + ": " + error_text();
        CloseHandle(h);
        return false;
    }
    SetupComm(h, 1 << 16, 1 << 16);
    port.fd = reinterpret_cast<intptr_t>(h);
    port.error.clear();
    return true;
}

void serial_close(SerialPort &port) noexcept
{
    if (serial_is_open(port))
    {
        CloseHandle(handle_of(port));
    }
    port.fd = -1;
}

bool serial_write(SerialPort &port, const void *data, size_t size)
{
    const auto *p = static_cast<const char *>(data);
    while (size > 0)
    {
        const long n = overlapped_io(port, [&](OVERLAPPED *ov) { return WriteFile(handle_of(port), p, static_cast<DWORD>(size), nullptr, ov) != 0; });
        if (n <= 0)
        {
            return false;
        }
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

long serial_read(SerialPort &port, void *data, size_t size, std::chrono::milliseconds timeout)
{
    // Only the listener thread reads, so changing the timeout here does not race another read.
    COMMTIMEOUTS to{.ReadIntervalTimeout = MAXDWORD, .ReadTotalTimeoutMultiplier = MAXDWORD,
                    .ReadTotalTimeoutConstant = static_cast<DWORD>(std::clamp<long long>(timeout.count(), 1, MAXDWORD - 1)),
                    .WriteTotalTimeoutMultiplier = 0, .WriteTotalTimeoutConstant = 0};
    if (!SetCommTimeouts(handle_of(port), &to))
    {
        port.error = error_text(); // an unplugged USB adapter fails here
        return -1;
    }
    return overlapped_io(port, [&](OVERLAPPED *ov) { return ReadFile(handle_of(port), data, static_cast<DWORD>(size), nullptr, ov) != 0; });
}

void serial_clear(SerialPort &port) noexcept
{
    PurgeComm(handle_of(port), PURGE_RXCLEAR | PURGE_TXCLEAR);
}

// "USB\VID_1A86&PID_55D3\5A1B2C3D": ids and the serial number (the last part, unless Windows made
// one up for a device without: those contain '&').
static void parse_instance_id(const std::string &id, SerialPortInfo &info)
{
    const auto hex_after = [&id](const char *key) -> uint16_t
    {
        const auto at = id.find(key);
        return at == std::string::npos ? 0 : static_cast<uint16_t>(std::strtoul(id.c_str() + at + 4, nullptr, 16));
    };
    info.vid = hex_after("VID_");
    info.pid = hex_after("PID_");
    const auto slash = id.rfind('\\');
    if (info.vid != 0 && slash != std::string::npos && id.find('&', slash) == std::string::npos)
    {
        info.serial = id.substr(slash + 1);
    }
}

std::vector<SerialPortInfo> serial_list_ports()
{
    std::vector<SerialPortInfo> ports;
    const HDEVINFO devs = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_COMPORT, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE)
    {
        return ports;
    }
    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof dev;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dev); ++i)
    {
        SerialPortInfo info;
        const HKEY key = SetupDiOpenDevRegKey(devs, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key != INVALID_HANDLE_VALUE)
        {
            char name[64] = {};
            DWORD size = sizeof name - 1;
            if (RegQueryValueExA(key, "PortName", nullptr, nullptr, reinterpret_cast<BYTE *>(name), &size) == ERROR_SUCCESS)
            {
                info.name = name;
            }
            RegCloseKey(key);
        }
        if (info.name.empty())
        {
            continue;
        }
        char id[512] = {};
        if (SetupDiGetDeviceInstanceIdA(devs, &dev, id, sizeof id - 1, nullptr))
        {
            parse_instance_id(id, info);
        }
        ports.push_back(std::move(info));
    }
    SetupDiDestroyDeviceInfoList(devs);
    return ports;
}

bool serial_is_open(const SerialPort &port) noexcept
{
    return port.fd != -1;
}
