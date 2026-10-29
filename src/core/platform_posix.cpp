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
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>   // _NSGetExecutablePath
#include <mach/mach.h>     // host_statistics64
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>       // pthread_mach_thread_np

extern char** environ;
#else
#include <sys/sendfile.h>
#endif

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
#ifdef __APPLE__
    // Reserve the blocks past the end of the file, then extend the file over them.
    fstore_t store{.fst_flags = F_ALLOCATEALL, .fst_posmode = F_PEOFPOSMODE, .fst_offset = 0,
                   .fst_length = static_cast<off_t>(to - from), .fst_bytesalloc = 0};
    if (fcntl(fd, F_PREALLOCATE, &store) != 0 && errno == ENOSPC)
    {
        return false;
    }
#else
    if (fallocate(fd, 0, static_cast<off_t>(from), static_cast<off_t>(to - from)) == 0)
    {
        return true;
    }
    if (errno != EOPNOTSUPP)
    {
        return false;
    }
#endif
    struct stat st{};
    return fstat(fd, &st) == 0
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
#ifdef __APPLE__
    return platform_no_file; // no memfd; the unified buffer cache keeps a fresh file in RAM anyway
#else
    return memfd_create("kraken-frame-cache", MFD_CLOEXEC);
#endif
}

PlatformFile file_dup(PlatformFile f)
{
    return fcntl(fd_of(f), F_DUPFD_CLOEXEC, 0);
}

bool file_copy_to(PlatformFile in, uint64_t size, const std::filesystem::path& out_path)
{
    const int out = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    off_t off = 0;
#ifdef __APPLE__ // no sendfile between files
    std::vector<char> buf(1 << 20);
    ssize_t n = 0;
    while (out >= 0 && static_cast<uint64_t>(off) < size
           && (n = pread(fd_of(in), buf.data(), std::min<uint64_t>(buf.size(), size - static_cast<uint64_t>(off)), off)) > 0
           && write(out, buf.data(), static_cast<std::size_t>(n)) == n)
    {
        off += n;
    }
#else
    while (out >= 0 && static_cast<uint64_t>(off) < size && sendfile(out, fd_of(in), &off, size - static_cast<uint64_t>(off)) > 0)
    {
    }
#endif
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
#ifdef __APPLE__
    char* const vec = reinterpret_cast<char*>(resident.data()); // char* there, unsigned char* on Linux
#else
    unsigned char* const vec = resident.data();
#endif
    const bool cached = mincore(static_cast<char*>(map) + off, size, vec) == 0
                        && std::ranges::all_of(resident, [](unsigned char r) { return (r & 1) != 0; });
    if (cached)
    {
        return true;
    }
    sink.resize(std::max(sink.size(), size));
    return pread(fd_of(f), sink.data(), size, static_cast<off_t>(off)) > 0;
}

// MemAvailable: free RAM plus the page cache the kernel can drop (sysconf's free pages leave the
// cache out, and after reading a big log nearly all RAM is cache). macOS: free, inactive and
// purgeable pages.
uint64_t platform_available_ram()
{
#ifdef __APPLE__
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) != KERN_SUCCESS)
    {
        return 0;
    }
    return (uint64_t{vm.free_count} + vm.inactive_count + vm.purgeable_count) * vm_page_size;
#else
    std::ifstream in("/proc/meminfo");
    std::string key;
    uint64_t kb = 0;
    while (in >> key >> kb && key != "MemAvailable:")
    {
        in.ignore(64, '\n');
    }
    return key == "MemAvailable:" ? kb * 1024 : 0;
#endif
}

int platform_pid()
{
    return getpid();
}

void platform_fine_timers()
{
#ifdef __APPLE__
    // XNU coalesces a thread's timers by its class: a background process (one launchd started, as a
    // CI runner is) gets up to 4x the wait, 100 ms at most, so a 10 ms cycle ran at ~37 ms. A
    // real-time thread gets no leeway, so the pacing thread asks for that policy for itself.
    mach_timebase_info_data_t tb{};
    mach_timebase_info(&tb);
    const auto ticks = [&](uint64_t ns) { return static_cast<uint32_t>(ns * tb.denom / tb.numer); };
    thread_time_constraint_policy_data_t policy{.period = ticks(1'000'000), .computation = ticks(100'000),
                                                .constraint = ticks(500'000), .preemptible = TRUE};
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#endif
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
#ifdef __APPLE__
    return home.empty() ? home : home / "Library/Application Support";
#else
    return home.empty() ? home : home / ".config";
#endif
}

std::filesystem::path platform_cache_dir()
{
    if (std::filesystem::path xdg = env_path("XDG_CACHE_HOME"); !xdg.empty())
    {
        return xdg;
    }
    const std::filesystem::path home = platform_home_dir();
#ifdef __APPLE__
    return home.empty() ? home : home / "Library/Caches";
#else
    return home.empty() ? home : home / ".cache";
#endif
}

std::vector<std::filesystem::path> platform_roots()
{
    return {"/"};
}

std::filesystem::path platform_exe_path()
{
    std::error_code ec;
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // asks for the size
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0)
    {
        return {};
    }
    path.resize(path.find('\0'));
    const std::filesystem::path exe = std::filesystem::canonical(path, ec);
    return ec ? std::filesystem::path{} : exe;
#else
    return std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
}

namespace
{

std::vector<char*> argv_of(std::span<const std::string> args)
{
    std::vector<char*> argv;
    for (const std::string& a : args)
    {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

} // namespace

int platform_run(std::span<const std::string> args)
{
    if (args.empty())
    {
        return -1;
    }
    std::vector<char*> argv = argv_of(args);
    pid_t pid = 0;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0)
    {
        return -1;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool platform_spawn(std::span<const std::string> args)
{
    if (args.empty())
    {
        return false;
    }
    std::vector<char*> argv = argv_of(args);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    pid_t pid = 0;
    const bool ok = posix_spawnp(&pid, argv[0], nullptr, &attr, argv.data(), environ) == 0;
    posix_spawnattr_destroy(&attr);
    if (ok)
    {
        std::thread([pid] { waitpid(pid, nullptr, 0); }).detach(); // no zombie while we run
    }
    return ok;
}
