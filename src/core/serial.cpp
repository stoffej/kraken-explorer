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
        ::cfsetispeed(&tio, speed);
        ::cfsetospeed(&tio, speed);
    }
    if (!ok || ::tcsetattr(fd, TCSANOW, &tio) != 0 || ::fcntl(fd, F_SETFL, 0) != 0)
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

void serial_clear(SerialPort &port) noexcept
{
    ::tcflush(port.fd, TCIOFLUSH);
}

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

bool serial_is_open(const SerialPort &port) noexcept
{
    return port.fd != -1;
}
