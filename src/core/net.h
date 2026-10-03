/*
  Copyright (c) 2026 Schildkroet

  This file is part of Kraken Explorer.

  Kraken Explorer is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  Kraken Explorer is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Kraken Explorer.  If not, see <http://www.gnu.org/licenses/>.
*/

// Sockets on both platforms: BSD sockets on Linux, Winsock 2 on Windows. A socket is an int here
// (-1: none; Winsock's SOCKET values fit in one), and what differs between the two is wrapped
// below. bind / listen / accept / send / recv / sendto / inet_pton are called as they are.

#pragma once

#include <cstddef>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// socket(2), close-on-exec; starts Winsock on first use.
[[nodiscard]] inline int net_socket(int domain, int type, int protocol)
{
#ifdef _WIN32
    static const int started = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d); }();
    return started == 0 ? static_cast<int>(socket(domain, type, protocol)) : -1;
#else
    return socket(domain, type | SOCK_CLOEXEC, protocol);
#endif
}

inline void net_close(int fd)
{
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(fd));
#else
    close(fd);
#endif
}

// Waits until fd is readable: > 0 readable (or hung up), 0 timeout or interrupted, < 0 error.
[[nodiscard]] inline int net_wait_readable(int fd, int timeout_ms)
{
#ifdef _WIN32
    WSAPOLLFD p{.fd = static_cast<SOCKET>(fd), .events = POLLIN, .revents = 0};
    return WSAPoll(&p, 1, timeout_ms);
#else
    pollfd p{.fd = fd, .events = POLLIN, .revents = 0};
    const int r = poll(&p, 1, timeout_ms);
    return r < 0 && errno == EINTR ? 0 : r;
#endif
}

inline int net_setsockopt(int fd, int level, int name, const void* value, std::size_t size)
{
    return setsockopt(fd, level, name, static_cast<const char*>(value), static_cast<socklen_t>(size));
}

// recvfrom(2); with dontwait it returns -1 instead of blocking when nothing is queued.
[[nodiscard]] inline long net_recvfrom(int fd, void* buf, std::size_t size, bool dontwait, sockaddr* from, socklen_t* from_len)
{
#ifdef _WIN32
    if (dontwait && net_wait_readable(fd, 0) <= 0) // no MSG_DONTWAIT in Winsock
    {
        return -1;
    }
    return recvfrom(static_cast<SOCKET>(fd), static_cast<char*>(buf), static_cast<int>(size), 0, from, from_len);
#else
    return recvfrom(fd, buf, size, dontwait ? MSG_DONTWAIT : 0, from, from_len);
#endif
}

// Text of the last socket error on this thread.
[[nodiscard]] inline std::string net_error_text()
{
#ifdef _WIN32
    return "Winsock error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}
