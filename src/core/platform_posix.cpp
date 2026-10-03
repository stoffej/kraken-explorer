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

#include "core/platform.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{

int fd_of(PlatformFile f) { return static_cast<int>(f); }

std::filesystem::path env_path(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0' ? std::filesystem::path(v) : std::filesystem::path{};
}

// Disk blocks for [from, to) of fd; a filesystem without fallocate just grows the file.
bool allocate(int fd, uint64_t from, uint64_t to)
{
    if (to <= from)
    {
        return true;
    }
    if (fallocate(fd, 0, static_cast<off_t>(from), static_cast<off_t>(to - from)) == 0)
    {
        return true;
    }
    struct stat st{};
    return errno == EOPNOTSUPP && fstat(fd, &st) == 0
           && (static_cast<uint64_t>(st.st_size) >= to || ftruncate(fd, static_cast<off_t>(to)) == 0);
}

} // namespace

PlatformFile file_open(const std::filesystem::path& p, FileMode mode)
{
    switch (mode)
    {
    case FileMode::read: return open(p.c_str(), O_RDONLY | O_CLOEXEC);
    case FileMode::read_write: return open(p.c_str(), O_RDWR | O_CLOEXEC);
    case FileMode::create: return open(p.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
    return platform_no_file;
}

void file_close(PlatformFile f)
{
    if (f != platform_no_file)
    {
        close(fd_of(f));
    }
}

bool file_read_at(PlatformFile f, void* data, std::size_t size, uint64_t off)
{
    return pread(fd_of(f), data, size, static_cast<off_t>(off)) == static_cast<ssize_t>(size);
}

bool file_write_at(PlatformFile f, const void* data, std::size_t size, uint64_t off)
{
    return pwrite(fd_of(f), data, size, static_cast<off_t>(off)) == static_cast<ssize_t>(size);
}

bool file_resize(PlatformFile f, uint64_t size)
{
    return ftruncate(fd_of(f), static_cast<off_t>(size)) == 0;
}

PlatformFile file_ram()
{
    return memfd_create("kraken-frame-cache", MFD_CLOEXEC);
}

PlatformFile file_dup(PlatformFile f)
{
    return fcntl(fd_of(f), F_DUPFD_CLOEXEC, 0);
}

bool file_copy_to(PlatformFile in, uint64_t size, const std::filesystem::path& out_path)
{
    const int out = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    off_t off = 0;
    while (out >= 0 && static_cast<uint64_t>(off) < size && sendfile(out, fd_of(in), &off, size - static_cast<uint64_t>(off)) > 0)
    {
    }
    return out >= 0 && close(out) == 0 && static_cast<uint64_t>(off) == size;
}

void* file_map(PlatformFile f, std::size_t size, bool write)
{
    void* p = size == 0 ? MAP_FAILED : mmap(nullptr, size, write ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd_of(f), 0);
    return p == MAP_FAILED ? nullptr : p;
}

void file_unmap(void* p, std::size_t size)
{
    if (p != nullptr)
    {
        munmap(p, size);
    }
}

bool grow_map_open(GrowMap& m, PlatformFile f, std::size_t reserve, bool ram)
{
    // One mapping far beyond the file (address space only); the file grows under it.
    void* p = mmap(nullptr, reserve, PROT_READ | PROT_WRITE, MAP_SHARED, fd_of(f), 0);
    if (p == MAP_FAILED)
    {
        return false;
    }
    m = {.base = static_cast<std::byte*>(p), .reserved = reserve, .size = 0, .file = f, .ram = ram, .views = {}};
    return true;
}

bool grow_map_grow(GrowMap& m, uint64_t size)
{
    if (size <= m.size)
    {
        return true;
    }
    if (size > m.reserved || (m.ram ? ftruncate(fd_of(m.file), static_cast<off_t>(size)) != 0 : !allocate(fd_of(m.file), m.size, size)))
    {
        return false;
    }
    m.size = size;
    return true;
}

void grow_map_close(GrowMap& m)
{
    if (m.base != nullptr)
    {
        munmap(m.base, m.reserved);
    }
    m = {};
}

void mem_sequential(void* p, std::size_t size)
{
    madvise(p, size, MADV_SEQUENTIAL);
}

void mem_release(const void* p, std::size_t size)
{
    const auto page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    const auto from = (reinterpret_cast<uintptr_t>(p) + page - 1) & ~(page - 1);
    const auto to = (reinterpret_cast<uintptr_t>(p) + size) & ~(page - 1);
    if (to > from)
    {
        madvise(reinterpret_cast<void*>(from), to - from, MADV_DONTNEED);
    }
}

bool mem_prefetch(PlatformFile f, void* map, uint64_t off, std::size_t size)
{
    // pread, not readahead(2): that one stays at read_ahead_kb-sized requests. A warm file (already
    // in the page cache) is not read again: copying it cost a warm build 12 %.
    static const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    thread_local std::vector<unsigned char> resident;
    thread_local std::vector<char> sink;
    resident.resize((size + page - 1) / page);
    const bool cached = mincore(static_cast<char*>(map) + off, size, resident.data()) == 0
                        && std::ranges::all_of(resident, [](unsigned char r) { return (r & 1) != 0; });
    if (cached)
    {
        return true;
    }
    sink.resize(std::max(sink.size(), size));
    return pread(fd_of(f), sink.data(), size, static_cast<off_t>(off)) > 0;
}

// MemAvailable: free RAM plus the page cache the kernel can drop (sysconf's free pages leave the
// cache out, and after reading a big log nearly all RAM is cache).
uint64_t platform_available_ram()
{
    std::ifstream in("/proc/meminfo");
    std::string key;
    uint64_t kb = 0;
    while (in >> key >> kb && key != "MemAvailable:")
    {
        in.ignore(64, '\n');
    }
    return key == "MemAvailable:" ? kb * 1024 : 0;
}

int platform_pid()
{
    return getpid();
}

void platform_fine_timers()
{
}

std::filesystem::path platform_home_dir()
{
    return env_path("HOME");
}

std::filesystem::path platform_config_dir()
{
    if (std::filesystem::path xdg = env_path("XDG_CONFIG_HOME"); !xdg.empty())
    {
        return xdg;
    }
    const std::filesystem::path home = platform_home_dir();
    return home.empty() ? home : home / ".config";
}

std::filesystem::path platform_cache_dir()
{
    if (std::filesystem::path xdg = env_path("XDG_CACHE_HOME"); !xdg.empty())
    {
        return xdg;
    }
    const std::filesystem::path home = platform_home_dir();
    return home.empty() ? home : home / ".cache";
}

std::vector<std::filesystem::path> platform_roots()
{
    return {"/"};
}

std::filesystem::path platform_exe_path()
{
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec);
}
