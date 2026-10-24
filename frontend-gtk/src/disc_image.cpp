// SPDX-License-Identifier: GPL-3.0-or-later
#include "disc_image.hpp"
#include <dreamstation/common/log.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <spdlog/fmt/fmt.h>

namespace ds::frontend {
namespace {

class GdiDiscImage final : public DiscImage {
public:
    explicit GdiDiscImage(std::vector<Track> tracks)
        : m_tracks(std::move(tracks)) {}

    std::span<const Track> tracks() const override { return m_tracks; }

    common::Result<void> read_sector(u32 lba,
                                     std::span<u8, 2048> out) override {
        const Track* track = find_track_for_lba(lba);
        if (!track) {
            return common::Error{
                fmt::format("LBA {} outside any track", lba)};
        }

        // Open lazily and cache; disc reads are sequential in practice.
        auto& stream = open_or_cached(track->file);
        if (!stream) {
            return common::Error{
                fmt::format("Failed to open {}", track->file.string())};
        }

        const u32 sector_in_track = lba - track->lba_start;
        const u64 file_pos =
            track->file_offset +
            static_cast<u64>(sector_in_track) * track->sector_size;

        // Raw 2352-byte sectors carry a 16-byte sync+header before the
        // 2048 bytes of user data, then 288 bytes of EDC/ECC after. Skip
        // the header to get the cooked payload.
        const u64 payload_offset =
            (track->sector_size == 2352) ? file_pos + 16 : file_pos;

        stream.seekg(static_cast<std::streamoff>(payload_offset));
        stream.read(reinterpret_cast<char*>(out.data()), 2048);

        if (stream.gcount() != 2048) {
            return common::Error{
                fmt::format("Short read at LBA {}", lba)};
        }
        return {};
    }

private:
    const Track* find_track_for_lba(u32 lba) const {
        for (const auto& t : m_tracks) {
            if (lba >= t.lba_start && lba < t.lba_start + t.length_sectors)
                return &t;
        }
        return nullptr;
    }

    std::ifstream& open_or_cached(const std::filesystem::path& p) {
        auto it = m_streams.find(p);
        if (it != m_streams.end()) return it->second;
        auto [ins, _] = m_streams.emplace(
            p, std::ifstream(p, std::ios::binary));
        return ins->second;
    }

    std::vector<Track> m_tracks;
    std::unordered_map<std::filesystem::path, std::ifstream> m_streams;
};

// Parse a single GDI line: "num lba type sector_size filename offset"
// Filenames may contain spaces and be quoted. We do a small hand-rolled
// tokenizer because the grammar is irregular.
common::Result<Track> parse_gdi_line(std::string_view line,
                                     const std::filesystem::path& base_dir) {
    Track t{};

    auto skip_ws = [&](size_t& i) {
        while (i < line.size() && std::isspace(static_cast<u8>(line[i]))) ++i;
    };
    auto read_uint = [&](size_t& i) -> common::Result<u32> {
        skip_ws(i);
        u32 v = 0;
        auto [ptr, ec] = std::from_chars(line.data() + i,
                                         line.data() + line.size(), v);
        if (ec != std::errc{}) return common::Error{"GDI: expected integer"};
        i = static_cast<size_t>(ptr - line.data());
        return v;
    };

    size_t i = 0;
    auto num         = read_uint(i); if (!num)         return num.error();
    auto lba         = read_uint(i); if (!lba)         return lba.error();
    auto type        = read_uint(i); if (!type)        return type.error();
    auto sector_size = read_uint(i); if (!sector_size) return sector_size.error();

    // Filename: may be quoted with double-quotes, or be a bare token.
    skip_ws(i);
    std::string filename;
    if (i < line.size() && line[i] == '"') {
        ++i;
        while (i < line.size() && line[i] != '"') filename.push_back(line[i++]);
        if (i < line.size()) ++i;   // closing quote
    } else {
        while (i < line.size() && !std::isspace(static_cast<u8>(line[i])))
            filename.push_back(line[i++]);
    }

    auto offset = read_uint(i); if (!offset) return offset.error();

    t.number      = *num;
    t.lba_start   = *lba;
    t.sector_size = *sector_size;
    t.type        = (*type == 4) ? TrackType::Data : TrackType::Audio;
    t.file        = base_dir / filename;
    t.file_offset = *offset;

    // length is computed after all tracks are loaded.
    return t;
}

common::Result<std::unique_ptr<DiscImage>>
load_gdi(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return common::Error{
        fmt::format("Cannot open GDI: {}", path.string())};

    u32 track_count = 0;
    in >> track_count;
    in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if (track_count == 0 || track_count > 99) {
        return common::Error{
            fmt::format("GDI: bogus track count {}", track_count)};
    }

    std::vector<Track> tracks;
    tracks.reserve(track_count);
    const auto base_dir = path.parent_path();

    std::string line;
    while (std::getline(in, line) && tracks.size() < track_count) {
        if (line.empty()) continue;
        auto t = parse_gdi_line(line, base_dir);
        if (!t) return t.error();
        tracks.push_back(*t);
    }

    if (tracks.size() != track_count) {
        return common::Error{"GDI: track count mismatch"};
    }

    // Compute each track's length from its file size.
    for (auto& t : tracks) {
        std::error_code ec;
        auto sz = std::filesystem::file_size(t.file, ec);
        if (ec) return common::Error{
            fmt::format("GDI: missing track file {}", t.file.string())};
        t.length_sectors = (sz - t.file_offset) / t.sector_size;
    }

    std::sort(tracks.begin(), tracks.end(),
              [](const Track& a, const Track& b) { return a.number < b.number; });

    ds::common::log()->info("Loaded GDI '{}': {} tracks, high-density boot LBA = 45000",
                            path.filename().string(), tracks.size());
    return std::unique_ptr<DiscImage>(
        std::make_unique<GdiDiscImage>(std::move(tracks)));
}

// ── CDI (DiscJuggler) loader ───────────────────────────────────────────────

// CDI version magic values (little-endian uint32 at byte [filesize-8])
constexpr u32 kCdiV2  = 0x80000004u;
constexpr u32 kCdiV35 = 0x80000006u;
constexpr u32 kCdiV4  = 0x80000007u;
constexpr u32 kCdiV41 = 0x80000008u;

class CdiDiscImage final : public DiscImage {
public:
    explicit CdiDiscImage(std::vector<Track> tracks,
                          std::filesystem::path cdi_path)
        : m_tracks(std::move(tracks))
        , m_path(std::move(cdi_path)) {}

    std::span<const Track> tracks() const override { return m_tracks; }

    common::Result<void> read_sector(u32 lba,
                                     std::span<u8, 2048> out) override {
        const Track* track = nullptr;
        for (const auto& t : m_tracks) {
            if (lba >= t.lba_start && lba < t.lba_start + t.length_sectors) {
                track = &t;
                break;
            }
        }
        if (!track)
            return common::Error{fmt::format("CDI: LBA {} outside any track", lba)};

        if (!m_stream.is_open()) {
            m_stream.open(m_path, std::ios::binary);
            if (!m_stream)
                return common::Error{fmt::format("CDI: cannot open {}", m_path.string())};
        }

        const u32 sector_in_track = lba - track->lba_start;
        const u64 raw_pos = track->file_offset +
                            static_cast<u64>(sector_in_track) * track->sector_size;
        // Skip 16-byte sync/header on raw 2352-byte sectors
        const u64 payload_pos = (track->sector_size == 2352) ? raw_pos + 16 : raw_pos;

        m_stream.seekg(static_cast<std::streamoff>(payload_pos));
        m_stream.read(reinterpret_cast<char*>(out.data()), 2048);
        if (m_stream.gcount() != 2048)
            return common::Error{fmt::format("CDI: short read at LBA {}", lba)};
        return {};
    }

private:
    std::vector<Track>    m_tracks;
    std::filesystem::path m_path;
    mutable std::ifstream m_stream;
};

// Read a little-endian u32 from a stream.
static bool read_u32(std::ifstream& f, u32& out) {
    u8 b[4];
    f.read(reinterpret_cast<char*>(b), 4);
    if (f.gcount() != 4) return false;
    out = u32(b[0]) | (u32(b[1]) << 8) | (u32(b[2]) << 16) | (u32(b[3]) << 24);
    return true;
}
static bool read_u16(std::ifstream& f, u16& out) {
    u8 b[2];
    f.read(reinterpret_cast<char*>(b), 2);
    if (f.gcount() != 2) return false;
    out = u16(b[0]) | (u16(b[1]) << 8);
    return true;
}

common::Result<std::unique_ptr<DiscImage>>
load_cdi(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return common::Error{fmt::format("Cannot open CDI: {}", path.string())};

    const u64 file_size = static_cast<u64>(f.tellg());
    if (file_size < 8)
        return common::Error{"CDI: file too small"};

    // Read 8-byte footer
    f.seekg(static_cast<std::streamoff>(file_size - 8));
    u32 version = 0, header_offset = 0;
    if (!read_u32(f, version) || !read_u32(f, header_offset))
        return common::Error{"CDI: cannot read footer"};

    if (version != kCdiV2  && version != kCdiV35 &&
        version != kCdiV4  && version != kCdiV41)
        return common::Error{fmt::format("CDI: unknown version 0x{:08X}", version)};

    if (header_offset == 0 || header_offset > file_size)
        return common::Error{"CDI: invalid header offset"};

    const u64 desc_pos = file_size - header_offset;
    f.seekg(static_cast<std::streamoff>(desc_pos));

    u16 session_count = 0;
    if (!read_u16(f, session_count) || session_count == 0 || session_count > 16)
        return common::Error{fmt::format("CDI: bad session count {}", session_count)};

    std::vector<Track> tracks;
    u64 file_offset = 0;  // running byte offset through the data section of the CDI
    u32 track_number = 0;

    for (u16 si = 0; si < session_count; ++si) {
        u16 track_count = 0;
        if (!read_u16(f, track_count) || track_count == 0 || track_count > 99)
            return common::Error{fmt::format("CDI: bad track count in session {}", si)};

        for (u16 ti = 0; ti < track_count; ++ti) {
            // Track record starts with 2×uint32 (usually zero), then filename_len
            u32 unk0 = 0, unk1 = 0, filename_len = 0;
            if (!read_u32(f, unk0) || !read_u32(f, unk1) || !read_u32(f, filename_len))
                return common::Error{"CDI: truncated track record"};
            if (filename_len > 256)
                return common::Error{"CDI: implausible filename length"};

            // Skip filename
            f.seekg(static_cast<std::streamoff>(filename_len), std::ios::cur);

            // 11 bytes unknown padding
            f.seekg(11, std::ios::cur);

            u32 mode = 0, session_track = 0, unk2 = 0, lba_start = 0;
            u32 total_length = 0, unk3 = 0;
            if (!read_u32(f, mode)         ||
                !read_u32(f, session_track)||
                !read_u32(f, unk2)         ||
                !read_u32(f, lba_start)    ||
                !read_u32(f, total_length) ||
                !read_u32(f, unk3))
                return common::Error{"CDI: truncated track fields"};

            // 16 bytes unknown
            f.seekg(16, std::ios::cur);

            u32 sector_size = 0;
            if (!read_u32(f, sector_size))
                return common::Error{"CDI: cannot read sector_size"};

            // 23 bytes unknown
            f.seekg(23, std::ios::cur);

            u32 total_length2 = 0;
            if (!read_u32(f, total_length2))
                return common::Error{"CDI: cannot read total_length2"};

            // v4+ has extra fields — skip a known amount
            if (version == kCdiV4 || version == kCdiV41)
                f.seekg(9, std::ios::cur);

            // Validate sector size
            if (sector_size != 2048 && sector_size != 2336 && sector_size != 2352)
                return common::Error{fmt::format(
                    "CDI: unsupported sector size {} in track {}", sector_size, track_number)};

            const u64 track_bytes = static_cast<u64>(total_length) * sector_size;

            Track t{};
            t.number         = ++track_number;
            t.lba_start      = lba_start;
            t.sector_size    = sector_size;
            t.type           = (mode == 0) ? TrackType::Audio : TrackType::Data;
            t.file           = path;       // data lives inside the CDI file
            t.file_offset    = file_offset;
            t.length_sectors = total_length;
            tracks.push_back(t);

            file_offset += track_bytes;
        }
    }

    if (tracks.empty())
        return common::Error{"CDI: no tracks found"};

    std::sort(tracks.begin(), tracks.end(),
              [](const Track& a, const Track& b) { return a.number < b.number; });

    ds::common::log()->info(
        "Loaded CDI '{}': {} sessions, {} tracks",
        path.filename().string(), session_count, tracks.size());

    return std::unique_ptr<DiscImage>(
        std::make_unique<CdiDiscImage>(std::move(tracks), path));
}

} // anonymous namespace

common::Result<std::unique_ptr<DiscImage>>
DiscImage::open(const std::filesystem::path& path) {
    const auto ext = path.extension().string();
    if (ext == ".gdi") return load_gdi(path);
    if (ext == ".cdi") return load_cdi(path);
    if (ext == ".chd") return common::Error{"CHD support: not yet implemented"};
    return common::Error{fmt::format("Unknown disc format: {}", ext)};
}

} // namespace ds::frontend
