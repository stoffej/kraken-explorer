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

// What differs between Linux and Windows, as free functions: files and mappings for the frame
// cache (ui/frame_cache.cpp), memory hints, well-known directories. platform_posix.cpp and
// platform_win32.cpp implement it; sockets are in core/net.h, serial ports in core/serial.h.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

// An open file: a file descriptor on Linux, a HANDLE on Windows.
using PlatformFile = intptr_t;
inline constexpr PlatformFile platform_no_file = -1;

enum class FileMode
{
    read,       // existing file, read only
    read_write, // existing file
    create,     // new or truncated, read-write
};

[[nodiscard]] PlatformFile file_open(const std::filesystem::path& p, FileMode mode);
void file_close(PlatformFile f);
// The whole range or false.
[[nodiscard]] bool file_read_at(PlatformFile f, void* data, std::size_t size, uint64_t off);
bool file_write_at(PlatformFile f, const void* data, std::size_t size, uint64_t off);
// Sets the file's size. Windows refuses while any mapping of the file exists.
[[nodiscard]] bool file_resize(PlatformFile f, uint64_t size);

// An anonymous file in RAM (Linux memfd), or platform_no_file where there is none (Windows: a
// pagefile section cannot grow, and mapped file writes are not throttled there anyway).
[[nodiscard]] PlatformFile file_ram();
// Another handle on the same file (survives file_close of f).
[[nodiscard]] PlatformFile file_dup(PlatformFile f);
// Copies the first `size` bytes of `in` to a new file `out` (Linux: sendfile, in the kernel).
[[nodiscard]] bool file_copy_to(PlatformFile in, uint64_t size, const std::filesystem::path& out);

// Maps [0, size) of f shared (writes reach the file); nullptr on failure. The mapping outlives
// file_close(f). On Windows a mapped file can be renamed but not deleted or replaced.
[[nodiscard]] void* file_map(PlatformFile f, std::size_t size, bool write);
void file_unmap(void* p, std::size_t size);

// A writable mapping of a file that grows: `reserved` bytes of address space from the start, so
// pointers into it stay valid while grow_map_grow extends the file under it.
struct GrowMap
{
    std::byte* base = nullptr;
    std::size_t reserved = 0;
    uint64_t size = 0; // [0, size) is backed by the file and writable
    PlatformFile file = platform_no_file;
    bool ram = false;          // file_ram(): grown without reserving disk blocks
    std::vector<void*> views;  // Windows: one view per grow step
};
[[nodiscard]] bool grow_map_open(GrowMap& m, PlatformFile f, std::size_t reserve, bool ram);
// The file is at least `size` bytes, really allocated (a full disk fails here, not as a fault on a
// mapped write later), and [0, size) is writable. May round size up (Windows: to 64 KB).
[[nodiscard]] bool grow_map_grow(GrowMap& m, uint64_t size);
void grow_map_close(GrowMap& m);

// Hints on a read-only file mapping. All best effort.
void mem_sequential(void* p, std::size_t size);        // will be read front to back
void mem_release(const void* p, std::size_t size);     // whole pages of the range are not needed again
// Brings [off, off + size) of the file mapped at `map` into the page cache with large reads
// (nothing is read when it is cached already). False on a read error.
bool mem_prefetch(PlatformFile f, void* map, uint64_t off, std::size_t size);

// RAM a new allocation can use without swapping: free plus droppable page cache.
[[nodiscard]] uint64_t platform_available_ram();
[[nodiscard]] int platform_pid();
// Timed waits wake within about a millisecond. Windows rounds them to its 15.6 ms tick unless a
// process asks; the threads that pace frames (cyclic transmit, replay) call this when they start.
void platform_fine_timers();

// Directories. $XDG_CONFIG_HOME / $XDG_CACHE_HOME win on both platforms when set (tests and
// benchmarks point them at scratch directories); otherwise ~/.config and ~/.cache on Linux,
// %APPDATA% and %LOCALAPPDATA% on Windows. Empty when nothing is known.
[[nodiscard]] std::filesystem::path platform_home_dir();
[[nodiscard]] std::filesystem::path platform_config_dir();
[[nodiscard]] std::filesystem::path platform_cache_dir();
// Where browsing the file system starts: "/" on Linux, the drives ("C:\\", ...) on Windows.
[[nodiscard]] std::vector<std::filesystem::path> platform_roots();
// The running executable; empty when unknown.
[[nodiscard]] std::filesystem::path platform_exe_path();
