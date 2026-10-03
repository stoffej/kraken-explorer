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

#include "ui/frame_cache.h"

#include "core/platform.h"
#include "core/setup.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <atomic>
#include <future>
#include <memory>
#include <numeric>
#include <optional>
#include <thread>

namespace
{

constexpr std::size_t block_size = std::size_t{1} << 20; // 16 MB: 1.05 s per warm 2 GB, 1 MB: 0.92 s (a block's frames stay in cache)

int64_t mtime_ns(const std::filesystem::path& p, std::error_code& ec)
{
    const auto t = std::filesystem::last_write_time(p, ec);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

uint64_t align8(uint64_t n) { return (n + 7) & ~uint64_t{7}; }

uint64_t row_key(const FrameCacheRec& r)
{
    const uint32_t id = r.errors != 0 ? replay_error_id : r.id;
    return (uint64_t{r.iface} << 32) | id;
}

// row_key -> row number, open addressing with linear probes: a per-frame std::unordered_map lookup
// was a third of a warm build. Keys never equal `empty` (iface is 16 bits).
struct RowIndex
{
    static constexpr uint64_t empty = UINT64_MAX;
    std::vector<uint64_t> keys = std::vector<uint64_t>(64, empty);
    std::vector<uint32_t> vals = std::vector<uint32_t>(64);
    std::size_t size = 0;

    // key's row, or `fresh` stored for it (second = true) when key is new.
    std::pair<uint32_t, bool> emplace(uint64_t key, uint32_t fresh)
    {
        const std::size_t mask = keys.size() - 1;
        for (std::size_t i = ((key * 0x9E3779B97F4A7C15ull) >> 40) & mask;; i = (i + 1) & mask)
        {
            if (keys[i] == key)
            {
                return {vals[i], false};
            }
            if (keys[i] == empty)
            {
                if (2 * (size + 1) > keys.size())
                {
                    grow();
                    return emplace(key, fresh);
                }
                keys[i] = key;
                vals[i] = fresh;
                ++size;
                return {fresh, true};
            }
        }
    }
    void grow()
    {
        RowIndex big;
        big.keys.assign(keys.size() * 2, empty);
        big.vals.resize(keys.size() * 2);
        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            if (keys[i] != empty)
            {
                big.emplace(keys[i], vals[i]);
            }
        }
        *this = std::move(big);
    }
    void clear()
    {
        std::ranges::fill(keys, empty);
        size = 0;
    }
};

// One parse worker's block: parsed, then encoded to records with the block's own channel numbers,
// payload indices and rows, so the main thread only renumbers and writes. Reused block after block.
struct BuildSlot
{
    ReplayFile f;
    std::vector<FrameCacheRec> recs;
    std::vector<FrameCachePayload> payloads; // of the recs with len > 8, indexed from 0
    std::vector<FrameCacheRow> rows;         // channel = the block's channel index
    RowIndex index;
    bool sorted = true;
    std::byte* write_to = nullptr; // set by the main thread: where recs go in the output mapping
};

// Copies a block's records to their place in the output (on the slot's worker, in parallel).
void write_block(BuildSlot& s)
{
    if (s.write_to != nullptr)
    {
        std::memcpy(s.write_to, s.recs.data(), s.recs.size() * sizeof(FrameCacheRec));
        s.write_to = nullptr;
    }
}

void encode_block(BuildSlot& s)
{
    s.recs.clear();
    s.payloads.clear();
    s.rows.clear();
    s.index.clear();
    s.sorted = true;
    s.recs.reserve(s.f.frames.size());
    int64_t last = INT64_MIN;
    for (const BusMessage& m : s.f.frames)
    {
        const FrameCacheRec& rec = s.recs.emplace_back(frame_cache_encode(m, static_cast<uint32_t>(s.payloads.size())));
        if (m.len > 8)
        {
            s.payloads.push_back(m.data);
        }
        const auto [r, added] = s.index.emplace(row_key(rec), static_cast<uint32_t>(s.rows.size()));
        if (added)
        {
            s.rows.push_back({.id = static_cast<uint32_t>(row_key(rec)), .channel = rec.iface,
                              .extended = static_cast<uint8_t>(has_flag(m, bus_flag::extended))});
        }
        ++s.rows[r].count;
        s.rows[r].dirs |= has_flag(m, bus_flag::tx) ? 2 : 1;
        s.sorted = s.sorted && m.ts_ns >= last;
        last = m.ts_ns;
    }
}

// A mapping of a whole file, unmapped on scope exit.
struct Mapping
{
    void* p = nullptr;
    std::size_t size = 0;
    Mapping() = default;
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    ~Mapping() { file_unmap(p, size); }
};

bool map_file(Mapping& m, PlatformFile f, std::size_t size, bool write)
{
    m.size = size;
    m.p = file_map(f, size, write);
    return m.p != nullptr;
}

// A file closed on scope exit.
struct File
{
    PlatformFile f = platform_no_file;
    explicit File(PlatformFile file) : f(file) {}
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File() { file_close(f); }
};

void grow_map_close_ptr(GrowMap* m) { grow_map_close(*m); }

// tmp becomes cache. A cache that is still mapped (the same log open in another tab or instance)
// cannot be replaced on Windows, only renamed: it is moved aside as a .tmp, which
// frame_cache_prune removes once nobody maps it. Each gets its own name: an earlier one may still
// be mapped. A few retries, for a virus scanner holding the new file for a moment.
bool replace_cache(const std::filesystem::path& tmp, const std::filesystem::path& cache)
{
    static std::atomic<unsigned> moved_aside{0};
    std::error_code ec;
    std::filesystem::rename(tmp, cache, ec);
    for (int attempt = 0; ec && attempt < 5; ++attempt)
    {
        std::filesystem::rename(cache, cache.string() + std::format(".{}.{}.old.tmp", platform_pid(), moved_aside++), ec);
        std::filesystem::rename(tmp, cache, ec);
        if (ec)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    return !ec;
}

uint32_t u32_at(std::string_view s, std::size_t off, bool big_endian)
{
    const auto* p = reinterpret_cast<const uint8_t*>(s.data() + off);
    return big_endian ? (uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3])
                      : (uint32_t{p[3]} << 24 | uint32_t{p[2]} << 16 | uint32_t{p[1]} << 8 | p[0]);
}

// Bytes of the pcap record at off (16-byte record header, incl_len at +8); 0 at the end or on a
// truncated record.
std::size_t pcap_record_size(std::string_view all, std::size_t off, bool big_endian)
{
    if (all.size() - off < 16)
    {
        return 0;
    }
    const std::size_t n = 16 + u32_at(all, off + 8, big_endian);
    return n <= all.size() - off ? n : 0;
}

// Bytes of the top-level BLF object at off, padding included (as parse_blf steps); 0 when off is
// not at an "LOBJ" or the object is cut off.
std::size_t blf_object_size(std::string_view all, std::size_t off)
{
    if (all.size() - off < 16 || all.substr(off, 4) != "LOBJ")
    {
        return 0;
    }
    const std::size_t size = u32_at(all, off + 8, false);
    if (size < 16 || size > all.size() - off)
    {
        return 0;
    }
    return std::min(size + size % 4, all.size() - off);
}

// The blocks a file is parsed (and hashed) in: text logs without header state split at line ends
// after every block_size bytes; pcap and BLF split between records (top-level objects: a BLF
// container stays whole, an object its stream continues in the next container is parsed from the
// two blocks' ReplayFile::blf_tail/blf_head) after every block_size bytes, the first block holding
// the file header that every block's parse gets (replay_header_size). TRC (its header defines the
// columns), pcapng (interface blocks anywhere) and MF4 are one block.
// ponytail: one block holds every frame of a pcapng/TRC/MF4 in RAM; pcapng could split between
// blocks with the SHB + IDBs seen so far passed along as its header.
std::vector<std::string_view> split_blocks(std::string_view all, TraceFileFormat format)
{
    const bool by_lines = format == TraceFileFormat::CanDump || format == TraceFileFormat::VectorAsc;
    const bool by_records = format == TraceFileFormat::Pcap || format == TraceFileFormat::Blf;
    const bool big_endian = format == TraceFileFormat::Pcap && !all.empty() && static_cast<uint8_t>(all[0]) == 0xA1;
    std::size_t record = by_records ? replay_header_size(all, format) : 0; // the record walk: next record boundary
    std::vector<std::string_view> blocks;
    for (std::size_t pos = 0; pos < all.size();)
    {
        std::size_t end = all.size();
        if (by_lines && all.size() - pos > block_size)
        {
            end = all.find('\n', pos + block_size);
            end = end == std::string_view::npos ? all.size() : end + 1;
        }
        else if (by_records && all.size() - pos > block_size)
        {
            // Records are walked header by header (no sync marker to seek to); a bad record ends
            // the walk and the rest is one block, parsed as before.
            while (record < pos + block_size)
            {
                const std::size_t n = format == TraceFileFormat::Pcap ? pcap_record_size(all, record, big_endian)
                                                                      : blf_object_size(all, record);
                if (n == 0)
                {
                    record = all.size();
                    break;
                }
                record += n;
            }
            end = record;
        }
        blocks.push_back(all.substr(pos, end - pos));
        pos = end;
    }
    return blocks;
}

// Four independent lanes of 8 bytes (one multiply chain was latency-bound at ~2 GB/s and took a
// quarter of a warm build), tail bytes folded in. Not cryptographic, only detects a changed log.
uint64_t block_hash(std::string_view block)
{
    constexpr uint64_t k = 0xBF58476D1CE4E5B9ull;
    uint64_t lane[4] = {0x9E3779B97F4A7C15ull ^ block.size(), 0xD1B54A32D192ED03ull, 0x8CB92BA72F3D8DD7ull, 0xA0761D6478BD642Full};
    const char* p = block.data();
    std::size_t n = block.size();
    for (; n >= 32; n -= 32, p += 32)
    {
        uint64_t w[4];
        std::memcpy(w, p, 32);
        for (int i = 0; i < 4; ++i)
        {
            lane[i] = (lane[i] ^ w[i]) * k;
            lane[i] ^= lane[i] >> 31;
        }
    }
    uint64_t tail[4] = {};
    std::memcpy(tail, p, n);
    uint64_t h = 0;
    for (int i = 0; i < 4; ++i)
    {
        h = (h ^ ((lane[i] ^ tail[i]) * k)) * 0x94D049BB133111EBull;
        h ^= h >> 29;
    }
    return h;
}

// The content hash: the blocks' hashes combined in order (FNV-1a over them). The build hashes each
// block in its parse worker while the bytes are in cache, so the file is read once.
uint64_t combine_hashes(std::span<const uint64_t> parts)
{
    uint64_t h = 1469598103934665603ull;
    for (const uint64_t part : parts)
    {
        h = (h ^ part) * 1099511628211ull;
    }
    return h;
}

// Hash of a whole file's content (mmap'ed), as the build stored it (blocks hashed on up to 8 threads).
std::optional<uint64_t> file_hash(const std::filesystem::path& p, uint64_t size)
{
    const TraceFileFormat format = trace_format_from_path(p.string()).value_or(TraceFileFormat::VectorAsc); // as the loader builds
    const File f(file_open(p, FileMode::read));
    Mapping m;
    if (f.f == platform_no_file || !map_file(m, f.f, size, false))
    {
        return std::nullopt;
    }
    const std::vector<std::string_view> blocks = split_blocks({static_cast<const char*>(m.p), size}, format);
    std::vector<uint64_t> parts(blocks.size());
    std::atomic<std::size_t> next{0};
    {
        std::vector<std::jthread> pool;
        for (std::size_t t = 0; t < std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8); ++t)
        {
            pool.emplace_back([&]
            {
                for (std::size_t i; (i = next++) < parts.size();)
                {
                    parts[i] = block_hash(blocks[i]);
                }
            });
        }
    } // joined
    return combine_hashes(parts);
}

// c's spans into its mapping, as laid out by h.
void set_views(FrameCache& c, const FrameCacheHeader& h)
{
    const auto* base = static_cast<const std::byte*>(c.map);
    c.recs = {reinterpret_cast<const FrameCacheRec*>(base + h.frames_off), h.frame_count};
    c.overflow = {reinterpret_cast<const FrameCachePayload*>(base + h.overflow_off), h.overflow_count};
    c.rows = {reinterpret_cast<const FrameCacheRow*>(base + h.rows_off), h.row_count};
    c.row_frames = {reinterpret_cast<const uint32_t*>(base + h.row_frames_off), h.frame_count};
    std::string_view names(reinterpret_cast<const char*>(base + h.channels_off), h.channels_size);
    while (!names.empty())
    {
        const auto nl = names.find('\n');
        c.channels.emplace_back(names.substr(0, nl));
        names = nl == std::string_view::npos ? std::string_view{} : names.substr(nl + 1);
    }
}

std::atomic<int> g_saving{0}; // background saves in flight

// Copies the in-memory cache (memfd) to tmp, then renames it to cache. Detached: the build has
// returned and the app already shows the file. An exit mid-save leaves a .tmp that
// frame_cache_prune removes; the next open builds again.
void save_in_background(PlatformFile memfd, uint64_t size, std::filesystem::path tmp, std::filesystem::path cache)
{
    const PlatformFile in = file_dup(memfd);
    if (in == platform_no_file)
    {
        return;
    }
    ++g_saving;
    std::thread([in, size, tmp = std::move(tmp), cache = std::move(cache)]
    {
        const bool ok = file_copy_to(in, size, tmp);
        file_close(in);
        if (ok && replace_cache(tmp, cache))
        {
            frame_cache_prune(cache.parent_path(), frame_cache_max_bytes, cache);
        }
        else
        {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
        }
        if (--g_saving == 0)
        {
            g_saving.notify_all();
        }
    }).detach();
}

} // namespace

void frame_cache_wait_saved()
{
    for (int n; (n = g_saving.load()) != 0;)
    {
        g_saving.wait(n);
    }
}

std::filesystem::path frame_cache_path(const std::filesystem::path& src)
{
    // One cache per file: a changed file (size, mtime in the header) rebuilds it in place.
    std::error_code ec;
    const std::string key = std::filesystem::weakly_canonical(src, ec).string();
    std::filesystem::path dir = platform_cache_dir();
    if (dir.empty())
    {
        dir = std::filesystem::temp_directory_path();
    }
    return dir / "kraken-explorer" / std::format("{:016x}.kfc", std::hash<std::string>{}(key));
}

std::expected<FrameCache, std::string> frame_cache_open(const std::filesystem::path& src, const std::filesystem::path& cache)
{
    std::error_code ec;
    const uint64_t src_size = std::filesystem::file_size(src, ec);
    const int64_t src_mtime = mtime_ns(src, ec);
    if (ec)
    {
        return std::unexpected("Cannot stat " + src.string());
    }
    File f(file_open(cache, FileMode::read_write)); // read-write to note a touched source's new mtime
    if (f.f == platform_no_file)
    {
        f.f = file_open(cache, FileMode::read);
    }
    FrameCacheHeader h;
    if (f.f == platform_no_file || !file_read_at(f.f, &h, sizeof h, 0))
    {
        return std::unexpected("No cache");
    }
    const FrameCacheHeader want;
    const uint64_t size = std::filesystem::file_size(cache, ec);
    if (std::memcmp(h.magic, want.magic, sizeof h.magic) != 0 || h.version != want.version || h.msg_size != want.msg_size)
    {
        return std::unexpected("Old cache"); // another format version: rebuilt, but the file did not change
    }
    const bool valid = h.src_size == src_size && !ec && h.channels_off + h.channels_size <= size;
    bool fresh = valid && h.src_mtime == src_mtime;
    if (valid && !fresh && file_hash(src, src_size) == h.src_hash)
    {
        fresh = true; // touched, same content
        h.src_mtime = src_mtime;
        file_write_at(f.f, &h, sizeof h, 0); // best effort: a read-only cache just hashes again next time
    }
    FrameCache c;
    c.map_size = size;
    if (!fresh)
    {
        return std::unexpected("Stale cache");
    }
    c.map = file_map(f.f, size, false); // the mapping keeps the file
    if (c.map == nullptr)
    {
        return std::unexpected("Cannot map cache");
    }
    std::filesystem::last_write_time(cache, std::filesystem::file_time_type::clock::now(), ec); // LRU mark for frame_cache_prune
    set_views(c, h);
    return c;
}

std::expected<FrameCache, std::string> frame_cache_build(const std::filesystem::path& src, const std::filesystem::path& cache,
                                                         TraceFileFormat format, const ReplayParseProgress& progress)
{
    std::error_code ec;
    FrameCacheHeader h;
    h.src_size = std::filesystem::file_size(src, ec);
    h.src_mtime = mtime_ns(src, ec);
    const File in(file_open(src, FileMode::read));
    Mapping data;
    if (ec || in.f == platform_no_file || !map_file(data, in.f, h.src_size, false))
    {
        return std::unexpected("Cannot open file.");
    }
    mem_sequential(data.p, data.size);

    std::filesystem::create_directories(cache.parent_path(), ec);
    const std::filesystem::path tmp = cache.string() + std::format(".{}.tmp", platform_pid());
    // Declared before the files and mappings: Windows cannot remove a file that is open or mapped.
    struct Remove
    {
        const std::filesystem::path& p;
        bool keep = false;
        ~Remove()
        {
            if (!keep)
            {
                std::error_code e;
                std::filesystem::remove(p, e);
            }
        }
    } remove_tmp{tmp};
    // In memory when the cache (about 0.8 x a candump, less for binary formats) fits in half the
    // free RAM: the disk then only has to keep up in the background. Windows has no growable RAM
    // file and does not throttle writes through a mapping, so it always builds in tmp.
    File out(h.src_size < platform_available_ram() / 2 ? file_ram() : platform_no_file);
    const bool in_ram = out.f != platform_no_file;
    if (!in_ram)
    {
        out.f = file_open(tmp, FileMode::create);
    }
    if (out.f == platform_no_file)
    {
        return std::unexpected("Cannot write " + tmp.string());
    }
    // Payloads longer than 8 bytes go to a second temporary file while the record count is still
    // unknown, and are copied after the records once it is (nothing for a classic CAN log).
    const std::filesystem::path tmp_overflow = std::filesystem::path(tmp).replace_extension(".ovf.tmp"); // pruned like tmp
    Remove remove_overflow{tmp_overflow};
    std::unique_ptr<FILE, int (*)(FILE*)> overflow(std::fopen(tmp_overflow.string().c_str(), "w+b"), &std::fclose);
    if (!overflow)
    {
        return std::unexpected("Cannot write " + tmp_overflow.string());
    }
    std::setvbuf(overflow.get(), nullptr, _IOFBF, std::size_t{1} << 20);

    // Pass 1: parse and encode blocks on worker threads, append them in file order.
    h.frames_off = 4096;
    std::vector<std::string> channels;
    std::vector<FrameCacheRow> rows;
    RowIndex row_of;
    bool sorted = true;
    int64_t last_ts = INT64_MIN;
    const std::string_view all(static_cast<const char*>(data.p), data.size);
    const std::vector<std::string_view> blocks = split_blocks(all, format);
    const std::string_view header = all.substr(0, replay_header_size(all, format)); // in blocks[0], which is parsed without it
    std::vector<uint64_t> hashes(blocks.size());
    // A sliding window of `threads` blocks in flight: as soon as the oldest is appended, its slot
    // parses the block `threads` further on (bounds the frames held in RAM, no wave barrier).
    const std::size_t threads = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 16); // flat beyond 12 (memory bound)
    std::size_t parsed_bytes = 0;
    std::vector<BuildSlot> slots(threads);
    std::vector<uint16_t> to_global;
    // The records go straight into the output through one mapping reserved far beyond any cache
    // (address space only), grown on disk as blocks come in: the workers copy their own records,
    // so the main thread no longer writes 1.5 GB per 2 GB of log through stdio.
    GrowMap out_map;
    if (!grow_map_open(out_map, out.f, std::size_t{1} << 40, in_ram))
    {
        return std::unexpected("Cannot map " + tmp.string());
    }
    const std::unique_ptr<GrowMap, void (*)(GrowMap*)> close_out_map(&out_map, &grow_map_close_ptr);
    // Appends an encoded block: channels and rows renumbered to the file's. False: write failed.
    const auto append = [&](BuildSlot& s)
    {
        if (s.recs.empty())
        {
            return true;
        }
        to_global.resize(s.f.channels.size());
        bool identity = true;
        for (std::size_t i = 0; i < s.f.channels.size(); ++i)
        {
            const auto it = std::ranges::find(channels, s.f.channels[i]);
            to_global[i] = static_cast<uint16_t>(it - channels.begin());
            identity = identity && to_global[i] == i;
            if (it == channels.end())
            {
                channels.push_back(s.f.channels[i]);
            }
        }
        for (FrameCacheRec& rec : identity && s.payloads.empty() ? std::span<FrameCacheRec>{} : std::span(s.recs))
        {
            if (!identity)
            {
                rec.iface = to_global[rec.iface];
            }
            if (rec.len > 8 && !s.payloads.empty())
            {
                uint32_t index = 0;
                std::memcpy(&index, rec.data, sizeof index);
                index += static_cast<uint32_t>(h.overflow_count);
                std::memcpy(rec.data, &index, sizeof index);
            }
        }
        if (!s.payloads.empty()
            && std::fwrite(s.payloads.data(), sizeof(FrameCachePayload), s.payloads.size(), overflow.get()) != s.payloads.size())
        {
            return false;
        }
        h.overflow_count += s.payloads.size();
        for (FrameCacheRow lr : s.rows)
        {
            lr.channel = to_global[lr.channel];
            const auto [g, added] = row_of.emplace((uint64_t{lr.channel} << 32) | lr.id, static_cast<uint32_t>(rows.size()));
            if (added)
            {
                rows.push_back(lr);
            }
            else
            {
                rows[g].count += lr.count;
                rows[g].dirs |= lr.dirs;
            }
        }
        sorted = sorted && s.sorted && s.recs.front().ts_ns >= last_ts;
        last_ts = s.recs.back().ts_ns;
        const uint64_t end = h.frames_off + (h.frame_count + s.recs.size()) * sizeof(FrameCacheRec);
        // Blocks really allocated (a full disk fails here, not as SIGBUS on a mapped write later).
        if (end > out_map.size && !grow_map_grow(out_map, std::max<uint64_t>(end, out_map.size * 2)))
        {
            return false;
        }
        s.write_to = out_map.base + h.frames_off + h.frame_count * sizeof(FrameCacheRec);
        h.frame_count += s.recs.size();
        return true;
    };
    // One thread reads the source ahead of the parse in big sequential chunks: 16 workers faulting
    // their blocks in made small reads (12 GB cold: 17.5 s -> 15 s, dd needs 12.6 s; two or four
    // reader threads measured no better).
    std::atomic<uint64_t> consumed{0}; // source bytes appended so far
    const std::jthread prefetch([&](const std::stop_token& stop)
    {
        constexpr uint64_t chunk = uint64_t{16} << 20;
        constexpr uint64_t window = uint64_t{1} << 30; // ponytail: fixed 1 GB ahead of the parse, tune if a faster disk outruns it
        for (uint64_t off = 0; off < h.src_size && !stop.stop_requested();)
        {
            if (off > consumed.load(std::memory_order_relaxed) + window)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (!mem_prefetch(in.f, data.p, off, static_cast<std::size_t>(std::min(chunk, h.src_size - off))))
            {
                break;
            }
            off += chunk;
        }
    });
    std::vector<std::future<void>> inflight(threads);
    const auto launch = [&](std::size_t k)
    {
        inflight[k % threads] = std::async(std::launch::async, [&, k]
        {
            BuildSlot& s = slots[k % threads];
            write_block(s); // the previous block of this slot
            hashes[k] = block_hash(blocks[k]); // first: the parse then reads the bytes from cache
            const std::string_view records = k == 0 ? blocks[0].substr(header.size()) : blocks[k];
            replay_parse_into(header, records, format, s.f, {.stop = progress.stop});
            mem_release(blocks[k].data(), blocks[k].size());
            encode_block(s);
            if (progress.frames != nullptr)
            {
                progress.frames->fetch_add(s.recs.size()); // counts up as blocks finish
            }
        });
    };
    for (std::size_t k = 0; k < std::min(threads, blocks.size()); ++k)
    {
        launch(k);
    }
    std::string tail; // BLF: the previous block's stream after its last whole object (an object's head)
    BuildSlot seam;
    for (std::size_t k = 0; k < blocks.size(); ++k)
    {
        inflight[k % threads].get();
        BuildSlot& s = slots[k % threads];
        if (progress.stop.stop_requested())
        {
            return std::unexpected("Cancelled"); // the other futures join in their destructors
        }
        if (!tail.empty() || !s.f.blf_head.empty())
        {
            // The object a block boundary cut inside a container stream: its head ended the
            // previous block, its tail starts this one. Parsed as a (top-level) object of its own.
            replay_parse_into(header, tail + s.f.blf_head, format, seam.f);
            encode_block(seam);
            if (!append(seam))
            {
                return std::unexpected("Write failed (disk full?)");
            }
            write_block(seam);
        }
        tail = std::move(s.f.blf_tail);
        if (!append(s))
        {
            return std::unexpected("Write failed (disk full?)");
        }
        if (k + threads < blocks.size())
        {
            launch(k + threads);
        }
        parsed_bytes += blocks[k].size();
        consumed.store(parsed_bytes, std::memory_order_relaxed);
        if (progress.fraction != nullptr)
        {
            progress.fraction->store(0.9f * static_cast<float>(static_cast<double>(parsed_bytes) / static_cast<double>(all.size())));
        }
    }
    {
        std::vector<std::jthread> last; // the window's last blocks were appended but not copied yet
        for (BuildSlot& s : slots)
        {
            last.emplace_back([&s] { write_block(s); });
        }
    }
    h.src_hash = combine_hashes(hashes);
    if (h.frame_count == 0)
    {
        return std::unexpected("Failed to parse trace file or file is empty.");
    }

    // Layout of the rest, then map the whole file and fill it in place.
    std::string names;
    for (const std::string& c : channels)
    {
        names += c + '\n';
    }
    h.overflow_off = align8(h.frames_off + h.frame_count * sizeof(FrameCacheRec));
    h.row_count = rows.size();
    h.rows_off = align8(h.overflow_off + h.overflow_count * sizeof(FrameCachePayload));
    h.row_frames_off = h.rows_off + h.row_count * sizeof(FrameCacheRow);
    h.channels_off = h.row_frames_off + h.frame_count * sizeof(uint32_t);
    h.channels_size = names.size();
    const uint64_t total = h.channels_off + h.channels_size;
    if (!grow_map_grow(out_map, total))
    {
        return std::unexpected("Write failed (disk full?)");
    }
    grow_map_close(out_map); // Windows resizes no file that is mapped
    Mapping map;
    if (!file_resize(out.f, total) || !map_file(map, out.f, total, true))
    {
        return std::unexpected("Cannot map " + tmp.string());
    }
    auto* base = static_cast<std::byte*>(map.p);
    std::rewind(overflow.get()); // the long payloads go after the records
    if (std::fread(base + h.overflow_off, sizeof(FrameCachePayload), h.overflow_count, overflow.get()) != h.overflow_count)
    {
        return std::unexpected("Write failed (disk full?)");
    }
    const std::span frames(reinterpret_cast<FrameCacheRec*>(base + h.frames_off), h.frame_count);
    if (!sorted)
    {
        std::ranges::stable_sort(frames, {}, &FrameCacheRec::ts_ns); // the overflow indices stay valid
    }

    // Pass 2: rows by (channel, id), each row's frame indices (counting sort, time order).
    std::vector<uint32_t> order(rows.size());
    std::iota(order.begin(), order.end(), 0u);
    std::ranges::sort(order, {}, [&](uint32_t i) { return std::pair{rows[i].channel, rows[i].id}; });
    const std::span out_rows(reinterpret_cast<FrameCacheRow*>(base + h.rows_off), h.row_count);
    std::vector<uint64_t> next(out_rows.size()); // per row: where its next frame index goes
    RowIndex sorted_row; // row_key -> row in out_rows
    uint64_t first = 0;
    for (std::size_t k = 0; k < order.size(); ++k)
    {
        out_rows[k] = rows[order[k]];
        out_rows[k].first = next[k] = first;
        first += out_rows[k].count;
        sorted_row.emplace((uint64_t{out_rows[k].channel} << 32) | out_rows[k].id, static_cast<uint32_t>(k));
    }
    auto* row_frames = reinterpret_cast<uint32_t*>(base + h.row_frames_off);
    // In parallel over slices of the frames: count each slice's frames per row, give every slice its
    // place in each row's list, fill the lists, then join the slices' cycle stats in order (the
    // cycle across a slice boundary is the previous slice's last frame to this one's first).
    struct Slice
    {
        std::vector<uint64_t> at;            // per row: count, then where the slice's next index goes
        std::vector<int64_t> first_ts, last_ts;
        std::vector<FrameCacheRow> cycles;   // per row: cycle_min / max / sum inside the slice
    };
    const std::size_t pieces = std::max<std::size_t>(1, std::min<std::size_t>(threads, frames.size() >> 16));
    std::vector<Slice> slices(pieces);
    const auto slice_range = [&](std::size_t t) { return std::pair{frames.size() * t / pieces, frames.size() * (t + 1) / pieces}; };
    const auto each_slice = [&](auto&& body)
    {
        std::vector<std::jthread> pool;
        for (std::size_t t = 0; t < pieces; ++t)
        {
            pool.emplace_back([&, t] { body(t); });
        }
    };
    each_slice([&](std::size_t t)
    {
        Slice& sl = slices[t];
        sl.at.assign(out_rows.size(), 0);
        RowIndex index = sorted_row;
        const auto [b, e] = slice_range(t);
        for (std::size_t i = b; i < e; ++i)
        {
            ++sl.at[index.emplace(row_key(frames[i]), 0).first];
        }
    });
    for (std::size_t k = 0; k < out_rows.size(); ++k)
    {
        for (Slice& sl : slices)
        {
            const uint64_t n = sl.at[k];
            sl.at[k] = next[k];
            next[k] += n;
        }
    }
    each_slice([&](std::size_t t)
    {
        Slice& sl = slices[t];
        sl.first_ts.assign(out_rows.size(), INT64_MIN);
        sl.last_ts.assign(out_rows.size(), INT64_MIN);
        sl.cycles.assign(out_rows.size(), FrameCacheRow{});
        RowIndex index = sorted_row;
        const auto [b, e] = slice_range(t);
        for (std::size_t i = b; i < e; ++i)
        {
            const uint32_t k = index.emplace(row_key(frames[i]), 0).first;
            row_frames[sl.at[k]++] = static_cast<uint32_t>(i);
            const int64_t ts = frames[i].ts_ns;
            if (sl.last_ts[k] == INT64_MIN)
            {
                sl.first_ts[k] = ts;
            }
            else
            {
                FrameCacheRow& c = sl.cycles[k];
                const int64_t cycle = ts - sl.last_ts[k]; // >= 0, frames are sorted
                c.cycle_min_ns = std::min(c.cycle_min_ns, cycle);
                c.cycle_max_ns = std::max(c.cycle_max_ns, cycle);
                c.cycle_sum_ns += cycle;
            }
            sl.last_ts[k] = ts;
        }
    });
    for (std::size_t k = 0; k < out_rows.size(); ++k)
    {
        FrameCacheRow& r = out_rows[k];
        int64_t last = INT64_MIN;
        for (const Slice& sl : slices)
        {
            if (sl.first_ts[k] == INT64_MIN)
            {
                continue;
            }
            const FrameCacheRow& c = sl.cycles[k];
            r.cycle_min_ns = std::min(r.cycle_min_ns, c.cycle_min_ns);
            r.cycle_max_ns = std::max(r.cycle_max_ns, c.cycle_max_ns);
            r.cycle_sum_ns += c.cycle_sum_ns;
            if (last != INT64_MIN)
            {
                const int64_t cycle = sl.first_ts[k] - last;
                r.cycle_min_ns = std::min(r.cycle_min_ns, cycle);
                r.cycle_max_ns = std::max(r.cycle_max_ns, cycle);
                r.cycle_sum_ns += cycle;
            }
            last = sl.last_ts[k];
        }
    }
    std::memcpy(base + h.channels_off, names.data(), names.size());
    std::memcpy(base, &h, sizeof h);
    if (progress.stop.stop_requested())
    {
        return std::unexpected("Cancelled");
    }
    FrameCache c;
    c.map_size = total;
    c.map = file_map(out.f, total, false);
    if (c.map == nullptr)
    {
        return std::unexpected("Cannot map " + tmp.string());
    }
    file_unmap(map.p, map.size);
    map.p = nullptr;
    set_views(c, h);
    if (in_ram)
    {
        remove_tmp.keep = true; // tmp is the background save's now
        save_in_background(out.f, total, tmp, cache);
    }
    else
    {
        file_close(out.f);
        out.f = platform_no_file;
        if (!replace_cache(tmp, cache))
        {
            frame_cache_close(c);
            return std::unexpected("Cannot write " + cache.string());
        }
        remove_tmp.keep = true;
        frame_cache_prune(cache.parent_path(), frame_cache_max_bytes, cache);
    }
    if (progress.fraction != nullptr)
    {
        progress.fraction->store(1.0f);
    }
    return c;
}

void frame_cache_prune(const std::filesystem::path& dir, uint64_t max_bytes, const std::filesystem::path& keep)
{
    struct Entry
    {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime;
        uint64_t size;
    };
    std::error_code ec;
    std::vector<Entry> caches;
    uint64_t total = 0;
    const auto now = std::filesystem::file_time_type::clock::now();
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    {
        const auto mtime = e.last_write_time(ec);
        if (e.path().extension() == ".tmp" && now - mtime > std::chrono::hours(1))
        {
            std::filesystem::remove(e.path(), ec); // a crashed build; a running one is newer
        }
        else if (e.path().extension() == ".kfc" && e.is_regular_file(ec))
        {
            caches.push_back({e.path(), mtime, e.file_size(ec)});
            total += caches.back().size;
        }
    }
    std::ranges::sort(caches, {}, &Entry::mtime); // mtime is "last opened": frame_cache_open touches it
    for (const Entry& e : caches)
    {
        if (total <= max_bytes)
        {
            break;
        }
        if (e.path.filename() != keep.filename() && std::filesystem::remove(e.path, ec))
        {
            total -= e.size;
        }
    }
}

void frame_cache_close(FrameCache& c)
{
    file_unmap(c.map, c.map_size);
    c = {};
}

std::vector<uint32_t> frame_cache_message_frames(const FrameCache& c, const Setup& setup, const CanDbMessage* msg)
{
    std::vector<uint32_t> out;
    std::size_t rows = 0;
    for (const FrameCacheRow& r : c.rows)
    {
        if (msg != nullptr && setup_find_can_message(setup, frame_cache_frame(c, c.row_frames[r.first])) == msg)
        {
            const auto list = c.row_frames.subspan(r.first, r.count);
            out.insert(out.end(), list.begin(), list.end());
            ++rows;
        }
    }
    if (rows > 1)
    {
        std::ranges::sort(out); // ponytail: sort instead of a k-way merge; several channels is the rare case
    }
    return out;
}
