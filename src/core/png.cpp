#include "core/png.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <string_view>

namespace
{

void put32(std::vector<uint8_t>& out, uint32_t v)
{
    out.insert(out.end(), {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8),
                           static_cast<uint8_t>(v)});
}

void chunk(std::vector<uint8_t>& out, std::string_view type, std::span<const uint8_t> data)
{
    put32(out, static_cast<uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), data.begin(), data.end());
    const std::span<const uint8_t> typed = std::span(out).subspan(start); // CRC over type + data
    put32(out, static_cast<uint32_t>(crc32(0, typed.data(), static_cast<uInt>(typed.size()))));
}

} // namespace

std::vector<uint8_t> png_encode(int w, int h, std::span<const uint8_t> rgba)
{
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    // Filter Up (2) on every row but the first (None): a plot is mostly flat areas and horizontal
    // runs, so the rows' differences are mostly zero and deflate well (4K graph: ~33 MB stored -> ~1 MB).
    std::vector<uint8_t> raw((stride + 1) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y)
    {
        uint8_t* row = raw.data() + static_cast<std::size_t>(y) * (stride + 1);
        const uint8_t* cur = rgba.data() + static_cast<std::size_t>(y) * stride;
        row[0] = y == 0 ? 0 : 2;
        for (std::size_t x = 0; x < stride; ++x)
        {
            row[1 + x] = y == 0 ? cur[x] : static_cast<uint8_t>(cur[x] - (cur - stride)[x]); // not cur[x - stride]: x - stride wraps
        }
    }
    uLongf size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> z(size);
    if (compress2(z.data(), &size, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
    {
        return {};
    }
    z.resize(size);
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(w));
    put32(ihdr, static_cast<uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8 bit, RGBA, deflate, no filter method, no interlace
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}

bool png_write_file(const std::filesystem::path& path, int w, int h, std::span<const uint8_t> rgba, std::string* error)
{
    const std::vector<uint8_t> bytes = png_encode(w, h, rgba);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out)
    {
        if (error != nullptr)
        {
            *error = std::format("cannot write {}", path.string());
        }
        return false;
    }
    return true;
}
