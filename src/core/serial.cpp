#include "serial.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>
#include <IOKit/serial/ioss.h> // IOSSIOSPEED
#include <sys/select.h>
#endif

#ifdef __APPLE__
// macOS has only the POSIX B* constants up to B230400: the port is configured at 9600 and the
// real rate set with IOSSIOSPEED afterwards (speed_t is the rate itself on Darwin).
static speed_t to_speed(int baud)
{
    switch (baud)
    {
    case 9600: case 19200: case 38400: case 57600: case 115200: case 230400: case 460800:
    case 500000: case 921600: case 1000000: case 2000000: case 3000000: case 4000000:
        return static_cast<speed_t>(baud);
    default: return B0;
    }
}
#else
static speed_t to_speed(int baud)
{
    switch (baud)
    {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 500000: return B500000;
    case 921600: return B921600;
    case 1000000: return B1000000;
    case 2000000: return B2000000;
    case 3000000: return B3000000;
    case 4000000: return B4000000;
    default: return B0;
    }
}
#endif

static std::string errno_text()
{
    return std::strerror(errno);
}

bool serial_open(SerialPort &port, const std::string &name, int baud)
{
    serial_close(port);
    const speed_t speed = to_speed(baud);
    if (speed == B0)
    {
        port.error = name + ": unsupported baud rate " + std::to_string(baud);
        return false;
    }
    // O_NONBLOCK so open() does not hang waiting for carrier detect; cleared below.
    const int fd = ::open(name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        port.error = name + ": " + errno_text();
        return false;
    }
    termios tio{};
    const bool ok = ::ioctl(fd, TIOCEXCL) == 0 && ::tcgetattr(fd, &tio) == 0;
    if (ok)
    {
        ::cfmakeraw(&tio);
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cflag &= ~(CSTOPB | CRTSCTS | PARENB);
        tio.c_cc[VMIN] = 0; // read() returns what is there; waiting is done with poll()
        tio.c_cc[VTIME] = 0;
#ifdef __APPLE__
        ::cfsetspeed(&tio, B9600);
#else
        ::cfsetispeed(&tio, speed);
        ::cfsetospeed(&tio, speed);
#endif
    }
    if (!ok || ::tcsetattr(fd, TCSANOW, &tio) != 0 || ::fcntl(fd, F_SETFL, 0) != 0
#ifdef __APPLE__
        || (::ioctl(fd, IOSSIOSPEED, &speed) != 0 && errno != ENOTTY) // ENOTTY: a pty, which has no rate
#endif
    )
    {
        port.error = name + ": " + errno_text();
        ::close(fd);
        return false;
    }
    port.fd = fd;
    port.error.clear();
    return true;
}

void serial_close(SerialPort &port) noexcept
{
    if (serial_is_open(port))
    {
        ::close(port.fd);
    }
    port.fd = -1;
}

bool serial_write(SerialPort &port, const void *data, size_t size)
{
    const auto *p = static_cast<const char *>(data);
    while (size > 0)
    {
        const ssize_t n = ::write(port.fd, p, size);
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n <= 0)
        {
            port.error = errno_text();
            return false;
        }
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

#ifdef __APPLE__
// select(), not poll(): Darwin's poll(2) "does not support devices" (BUGS), and a tty is one.
long serial_read(SerialPort &port, void *data, size_t size, std::chrono::milliseconds timeout)
{
    if (port.fd >= FD_SETSIZE)
    {
        port.error = "file descriptor too large for select()";
        return -1;
    }
    fd_set in;
    FD_ZERO(&in);
    FD_SET(port.fd, &in);
    timeval tv{.tv_sec = static_cast<time_t>(timeout.count() / 1000),
               .tv_usec = static_cast<suseconds_t>(timeout.count() % 1000 * 1000)};
    const int r = ::select(port.fd + 1, &in, nullptr, nullptr, &tv);
    if (r == 0 || (r < 0 && errno == EINTR))
    {
        return 0;
    }
    if (r < 0)
    {
        port.error = errno_text();
        return -1;
    }
    const ssize_t n = ::read(port.fd, data, size);
    if (n < 0)
    {
        port.error = errno_text();
        return -1;
    }
    if (n == 0) // readable without data: the device is gone
    {
        port.error = "device disconnected";
        return -1;
    }
    return static_cast<long>(n);
}
#else
long serial_read(SerialPort &port, void *data, size_t size, std::chrono::milliseconds timeout)
{
    pollfd pfd{.fd = static_cast<int>(port.fd), .events = POLLIN, .revents = 0};
    const int r = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
    if (r == 0 || (r < 0 && errno == EINTR))
    {
        return 0;
    }
    if (r < 0 || (pfd.revents & (POLLERR | POLLNVAL)))
    {
        port.error = r < 0 ? errno_text() : "port error";
        return -1;
    }
    const ssize_t n = ::read(pfd.fd, data, size);
    if (n < 0)
    {
        port.error = errno_text();
        return -1;
    }
    if (n == 0 && (pfd.revents & POLLHUP))
    {
        port.error = "device disconnected";
        return -1;
    }
    return static_cast<long>(n);
}
#endif

void serial_clear(SerialPort &port) noexcept
{
    ::tcflush(port.fd, TCIFLUSH);
}

#ifdef __APPLE__
// A string or number property of a registry entry; opts searches up the parents (the USB
// device a serial interface belongs to). Empty or 0 when there is none.
static std::string registry_string(io_object_t entry, CFStringRef key, IOOptionBits opts)
{
    std::string out;
    CFTypeRef v = IORegistryEntrySearchCFProperty(entry, kIOServicePlane, key, kCFAllocatorDefault, opts);
    if (v == nullptr)
    {
        return out;
    }
    char buf[1024];
    if (CFGetTypeID(v) == CFStringGetTypeID() && CFStringGetCString(static_cast<CFStringRef>(v), buf, sizeof buf, kCFStringEncodingUTF8))
    {
        out = buf;
    }
    CFRelease(v);
    return out;
}

static uint16_t registry_u16(io_object_t entry, CFStringRef key, IOOptionBits opts)
{
    int value = 0;
    CFTypeRef v = IORegistryEntrySearchCFProperty(entry, kIOServicePlane, key, kCFAllocatorDefault, opts);
    if (v == nullptr)
    {
        return 0;
    }
    if (CFGetTypeID(v) == CFNumberGetTypeID())
    {
        CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberIntType, &value);
    }
    CFRelease(v);
    return static_cast<uint16_t>(value);
}

// Every IOSerialBSDClient by its callout device (/dev/cu.*: unlike /dev/tty.* its open() does
// not wait for carrier detect), with VID/PID/serial of the USB device above it.
std::vector<SerialPortInfo> serial_list_ports()
{
    std::vector<SerialPortInfo> ports;
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(kIOSerialBSDServiceValue), &it) != KERN_SUCCESS)
    {
        return ports;
    }
    const IOOptionBits up = kIORegistryIterateRecursively | kIORegistryIterateParents;
    while (const io_object_t service = IOIteratorNext(it))
    {
        SerialPortInfo info;
        info.name = registry_string(service, CFSTR(kIOCalloutDeviceKey), 0);
        info.vid = registry_u16(service, CFSTR("idVendor"), up);
        info.pid = registry_u16(service, CFSTR("idProduct"), up);
        info.serial = registry_string(service, CFSTR("USB Serial Number"), up);
        IOObjectRelease(service);
        if (!info.name.empty())
        {
            ports.push_back(std::move(info));
        }
    }
    IOObjectRelease(it);
    return ports;
}
#else
static std::string read_line(const std::filesystem::path &p)
{
    std::ifstream f(p);
    std::string s;
    std::getline(f, s);
    return s;
}

std::vector<SerialPortInfo> serial_list_ports()
{
    namespace fs = std::filesystem;
    std::vector<SerialPortInfo> ports;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator("/sys/class/tty", ec))
    {
        const fs::path dev = e.path() / "device";
        if (!fs::exists(dev, ec))
        {
            continue; // virtual terminals
        }
        // The 32 legacy ttyS placeholders: 8250 ports with no UART behind them (type 0 = PORT_UNKNOWN).
        // Checked via the device path, since newer kernels bind them to the serial-base "port" driver.
        const fs::path real = fs::canonical(dev, ec);
        if (real.string().find("serial8250") != std::string::npos && read_line(e.path() / "type") == "0")
        {
            continue;
        }
        SerialPortInfo info;
        info.name = "/dev/" + e.path().filename().string();
        // USB interface dir -> its parent USB device dir holds idVendor/idProduct/serial.
        for (fs::path p = real; !ec && p.has_relative_path(); p = p.parent_path())
        {
            if (fs::exists(p / "idVendor", ec))
            {
                info.vid = static_cast<uint16_t>(std::stoul("0" + read_line(p / "idVendor"), nullptr, 16));
                info.pid = static_cast<uint16_t>(std::stoul("0" + read_line(p / "idProduct"), nullptr, 16));
                info.serial = read_line(p / "serial");
                break;
            }
        }
        ports.push_back(std::move(info));
    }
    return ports;
}
#endif

bool serial_is_open(const SerialPort &port) noexcept
{
    return port.fd != -1;
}
