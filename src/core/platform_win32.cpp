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
#include <cstdlib>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <mmsystem.h> // timeBeginPeriod

namespace
{

HANDLE handle_of(PlatformFile f) { return reinterpret_cast<HANDLE>(f); }

std::filesystem::path env_path(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0' ? std::filesystem::path(v) : std::filesystem::path{};
}

constexpr uint64_t granularity = 64 * 1024; // views start at multiples of the allocation granularity

// Placeholder API (Windows 10 1803): looked up at run time, MinGW has no import library for it.
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
constexpr ULONG mem_reserve_placeholder = 0x00040000;
constexpr ULONG mem_replace_placeholder = 0x00004000;
constexpr ULONG mem_preserve_placeholder = 0x00000002;

template <typename Fn>
Fn kernelbase(const char* name)
{
    const HMODULE dll = GetModuleHandleW(L"kernelbase.dll");
    return dll != nullptr ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(dll, name))) : nullptr;
}

} // namespace

PlatformFile file_open(const std::filesystem::path& p, FileMode mode)
{
    // Share everything, delete included: a cache is renamed into place while it is mapped.
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const DWORD access = mode == FileMode::read ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;
    const HANDLE h = CreateFileW(p.c_str(), access, share, nullptr, mode == FileMode::create ? CREATE_ALWAYS : OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    return h == INVALID_HANDLE_VALUE ? platform_no_file : reinterpret_cast<PlatformFile>(h);
}

void file_close(PlatformFile f)
{
    if (f != platform_no_file)
    {
        CloseHandle(handle_of(f));
    }
}

bool file_read_at(PlatformFile f, void* data, std::size_t size, uint64_t off)
{
    auto* p = static_cast<char*>(data);
    while (size > 0)
    {
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(off);
        at.OffsetHigh = static_cast<DWORD>(off >> 32);
        DWORD n = 0;
        if (!ReadFile(handle_of(f), p, static_cast<DWORD>(std::min<std::size_t>(size, 1u << 30)), &n, &at) || n == 0)
        {
            return false;
        }
        p += n;
        off += n;
        size -= n;
    }
    return true;
}

bool file_write_at(PlatformFile f, const void* data, std::size_t size, uint64_t off)
{
    const auto* p = static_cast<const char*>(data);
    while (size > 0)
    {
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(off);
        at.OffsetHigh = static_cast<DWORD>(off >> 32);
        DWORD n = 0;
        if (!WriteFile(handle_of(f), p, static_cast<DWORD>(std::min<std::size_t>(size, 1u << 30)), &n, &at) || n == 0)
        {
            return false;
        }
        p += n;
        off += n;
        size -= n;
    }
    return true;
}

bool file_resize(PlatformFile f, uint64_t size)
{
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    return SetFileInformationByHandle(handle_of(f), FileEndOfFileInfo, &info, sizeof info) != 0;
}

PlatformFile file_ram()
{
    return platform_no_file;
}

PlatformFile file_dup(PlatformFile f)
{
    HANDLE out = nullptr;
    const HANDLE self = GetCurrentProcess();
    return DuplicateHandle(self, handle_of(f), self, &out, 0, FALSE, DUPLICATE_SAME_ACCESS) ? reinterpret_cast<PlatformFile>(out)
                                                                                           : platform_no_file;
}

bool file_copy_to(PlatformFile in, uint64_t size, const std::filesystem::path& out_path)
{
    // Only the RAM-built cache is saved this way, and file_ram() has none here: a plain copy loop.
    const PlatformFile out = file_open(out_path, FileMode::create);
    std::vector<char> buf(std::size_t{4} << 20);
    bool ok = out != platform_no_file;
    for (uint64_t off = 0; ok && off < size; off += buf.size())
    {
        const auto n = static_cast<std::size_t>(std::min<uint64_t>(buf.size(), size - off));
        ok = file_read_at(in, buf.data(), n, off) && file_write_at(out, buf.data(), n, off);
    }
    file_close(out);
    return ok;
}

void* file_map(PlatformFile f, std::size_t size, bool write)
{
    if (size == 0)
    {
        return nullptr;
    }
    const HANDLE section = CreateFileMappingW(handle_of(f), nullptr, write ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
    if (section == nullptr)
    {
        return nullptr;
    }
    void* p = MapViewOfFile(section, write ? FILE_MAP_READ | FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, size);
    CloseHandle(section); // the view keeps the section
    return p;
}

void file_unmap(void* p, std::size_t)
{
    if (p != nullptr)
    {
        UnmapViewOfFile(p);
    }
}

bool grow_map_open(GrowMap& m, PlatformFile f, std::size_t reserve, bool ram)
{
    // A placeholder reserves the address space; every grow step replaces its next part with a
    // view of the (longer) file, so the views are contiguous.
    static const auto virtual_alloc2 = kernelbase<VirtualAlloc2Fn>("VirtualAlloc2");
    void* p = virtual_alloc2 != nullptr
                  ? virtual_alloc2(nullptr, nullptr, reserve, MEM_RESERVE | mem_reserve_placeholder, PAGE_NOACCESS, nullptr, 0)
                  : nullptr;
    if (p == nullptr)
    {
        return false;
    }
    m = {.base = static_cast<std::byte*>(p), .reserved = reserve, .size = 0, .file = f, .ram = ram, .views = {}};
    return true;
}

bool grow_map_grow(GrowMap& m, uint64_t size)
{
    static const auto map_view3 = kernelbase<MapViewOfFile3Fn>("MapViewOfFile3");
    size = (size + granularity - 1) & ~(granularity - 1);
    if (size <= m.size)
    {
        return true;
    }
    if (map_view3 == nullptr || size >= m.reserved)
    {
        return false;
    }
    // Creating the section extends the file (NTFS allocates the clusters: a full disk fails here).
    const HANDLE section = CreateFileMappingW(handle_of(m.file), nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32),
                                              static_cast<DWORD>(size), nullptr);
    if (section == nullptr)
    {
        return false;
    }
    std::byte* at = m.base + m.size;
    const std::size_t len = static_cast<std::size_t>(size - m.size);
    // Split [at, at + len) off the placeholder, then put the view there.
    void* view = VirtualFree(at, len, MEM_RELEASE | mem_preserve_placeholder)
                     ? map_view3(section, nullptr, at, m.size, len, mem_replace_placeholder, PAGE_READWRITE, nullptr, 0)
                     : nullptr;
    CloseHandle(section);
    if (view == nullptr)
    {
        return false;
    }
    m.views.push_back(view);
    m.size = size;
    return true;
}

void grow_map_close(GrowMap& m)
{
    for (void* view : m.views)
    {
        UnmapViewOfFile(view);
    }
    if (m.base != nullptr)
    {
        VirtualFree(m.base + m.size, 0, MEM_RELEASE); // what is left of the placeholder
    }
    m = {};
}

void mem_sequential(void*, std::size_t)
{
}

void mem_release(const void* p, std::size_t size)
{
    // Unlocking pages that are not locked removes them from the working set (they stay in the
    // standby list, which the system reuses first).
    constexpr uintptr_t page = 4096;
    const auto from = (reinterpret_cast<uintptr_t>(p) + page - 1) & ~(page - 1);
    const auto to = (reinterpret_cast<uintptr_t>(p) + size) & ~(page - 1);
    if (to > from)
    {
        VirtualUnlock(reinterpret_cast<void*>(from), to - from);
    }
}

bool mem_prefetch(PlatformFile, void* map, uint64_t off, std::size_t size)
{
    WIN32_MEMORY_RANGE_ENTRY range{.VirtualAddress = static_cast<char*>(map) + off, .NumberOfBytes = size};
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0); // big reads; nothing for cached pages
    return true; // a failed hint is not a read error: the parse faults the pages in itself
}

uint64_t platform_available_ram()
{
    MEMORYSTATUSEX st{};
    st.dwLength = sizeof st;
    return GlobalMemoryStatusEx(&st) ? st.ullAvailPhys : 0; // free + standby (the droppable cache)
}

int platform_pid()
{
    return static_cast<int>(GetCurrentProcessId());
}

void platform_fine_timers()
{
    static const MMRESULT once = []
    {
        // Windows 11 ignores timeBeginPeriod while the window is minimized or covered unless the
        // process opts out of that throttling: cyclic TX and Replay would fall back to 15.6 ms.
        // ProcessPowerThrottling = 4, PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION = 4;
        // older Windows refuses the call, which changes nothing there.
        struct { ULONG version, control_mask, state_mask; } throttling{1, 4, 0};
        SetProcessInformation(GetCurrentProcess(), static_cast<PROCESS_INFORMATION_CLASS>(4), &throttling, sizeof throttling);
        return timeBeginPeriod(1); // for the life of the process
    }();
    (void)once;
}

std::filesystem::path platform_home_dir()
{
    std::filesystem::path home = env_path("HOME");
    return home.empty() ? env_path("USERPROFILE") : home;
}

std::filesystem::path platform_config_dir()
{
    std::filesystem::path xdg = env_path("XDG_CONFIG_HOME");
    return xdg.empty() ? env_path("APPDATA") : xdg;
}

std::filesystem::path platform_cache_dir()
{
    std::filesystem::path xdg = env_path("XDG_CACHE_HOME");
    return xdg.empty() ? env_path("LOCALAPPDATA") : xdg;
}

std::vector<std::filesystem::path> platform_roots()
{
    std::vector<std::filesystem::path> roots;
    const DWORD drives = GetLogicalDrives(); // bit 0: A:
    for (int i = 0; i < 26; ++i)
    {
        if ((drives >> i) & 1)
        {
            roots.emplace_back(std::string{static_cast<char>('A' + i), ':', '\\'});
        }
    }
    return roots;
}

std::filesystem::path platform_exe_path()
{
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return buf;
}
