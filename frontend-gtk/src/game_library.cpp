// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_library.hpp"
#include <dreamstation/common/log.hpp>
#include <dreamstation/common/types.hpp>
#include "disc_image.hpp"
#include <dreamstation/engine/engine.hpp>
#include <algorithm>
#include <set>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <glib.h>

namespace ds::frontend {

namespace {
constexpr u32 kIpBinLba = 45000;

std::string lower(std::string s);

std::string lower_ext(const std::filesystem::path& p) {
    auto ext = p.extension().string();
    for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

bool has_disc_ext(const std::filesystem::path& p) {
    const auto ext = lower_ext(p);
    return ext == ".gdi" || ext == ".cdi" || ext == ".chd" || ext == ".cue";
}

// An archive is an arcade game only if the engine knows a set by that name.
bool is_arcade_set(const std::filesystem::path& p) {
    const auto ext = lower_ext(p);
    if (ext != ".zip" && ext != ".7z")
        return false;
    return !ds::engine::arcade_title(p.stem().string()).empty();
}

// A raw Naomi cartridge dump (.dat/.bin) starts with the board's name.
// Checking it keeps disc track files, which are also .bin, out of the list.
// The engine runs these dumps on a Naomi only, so other boards' dumps are
// left out rather than listed and failing.
bool is_naomi_cart(const std::filesystem::path& p) {
    const auto ext = lower_ext(p);
    if (ext != ".dat" && ext != ".bin")
        return false;
    char header[5] = {};
    std::ifstream in(p, std::ios::binary);
    if (!in.read(header, sizeof(header)))
        return false;
    return std::memcmp(header, "NAOMI", 5) == 0;
}

// What an unzipped set folder lacks to run, or empty when it is complete.
// Files are looked for here and in the parent set's folder beside it, which
// is how MAME sets are laid out. A ROM is found by name; failing that, like
// the engine, by checksum among the files no other ROM claims by name (only
// those are read, so a scan stays fast).
std::string missing_from_set_folder(const std::filesystem::path& dir,
                                    const ds::engine::ArcadeSet& set) {
    namespace fs = std::filesystem;
    std::vector<fs::path> folders{dir};
    if (!set.parent.empty())
        folders.push_back(dir.parent_path() / set.parent);
    std::map<std::string, fs::path> files;   // lower-case name -> path
    std::error_code ec;
    for (const auto& folder : folders)
        for (const auto& e : fs::directory_iterator(folder, ec))
            if (e.is_regular_file(ec))
                files.emplace(lower(e.path().filename().string()), e.path());

    std::vector<const ds::engine::ArcadeSet::Rom*> unnamed;
    for (const auto& rom : set.roms)
        if (!rom.optional && !files.count(lower(rom.file_name)))
            unnamed.push_back(&rom);
    if (!unnamed.empty()) {
        // Only files no ROM asks for by name can be a renamed one.
        for (const auto& rom : set.roms)
            files.erase(lower(rom.file_name));
        for (const auto* rom : unnamed)
            if (std::none_of(files.begin(), files.end(), [&](const auto& f) {
                    return ds::engine::arcade_rom_matches(f.second, *rom);
                }))
                return rom->file_name;
    }

    if (!set.gdrom.empty()) {
        const std::string image = set.gdrom + ".chd";
        if (!std::any_of(folders.begin(), folders.end(),
                         [&](const fs::path& f) { return fs::is_regular_file(f / image, ec); }))
            return image;
    }
    return {};
}

std::uintmax_t folder_size(const std::filesystem::path& dir) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_regular_file(ec))
            total += e.file_size(ec);
    return total;
}

// "mobile_suit_gundam" -> "Mobile Suit Gundam"; names with spaces are kept.
std::string title_from_file_name(std::string name) {
    if (name.find(' ') != std::string::npos)
        return name;
    bool start = true;
    for (auto& c : name) {
        if (c == '_') { c = ' '; start = true; continue; }
        if (start) c = char(std::toupper(static_cast<unsigned char>(c)));
        start = false;
    }
    return name;
}

std::uintmax_t size_on_disk(const std::filesystem::path& p) {
    namespace fs = std::filesystem;
    std::error_code ec;
    // A GDI/CUE is a small index; the data is in the track files beside it.
    const auto ext = lower_ext(p);
    if (ext == ".gdi" || ext == ".cue") {
        std::uintmax_t total = 0;
        for (const auto& e : fs::directory_iterator(p.parent_path(), ec)) {
            if (!e.is_regular_file(ec)) continue;
            const auto x = lower_ext(e.path());
            if (x == ".bin" || x == ".raw" || x == ".iso" || x == ".gdi" || x == ".cue")
                total += e.file_size(ec);
        }
        return total;
    }
    const auto size = fs::file_size(p, ec);
    return ec ? 0 : size;
}

// Converts arbitrary bytes to a valid UTF-8 string safe to pass to Pango/GTK.
// Tries UTF-8 first, then Shift-JIS (common for Japanese Dreamcast titles),
// then falls back to keeping ASCII and replacing non-ASCII with '?'.
std::string sanitize_to_utf8(const char* buf, std::size_t len) {
    // Trim trailing nulls and padding spaces
    while (len > 0 && (buf[len-1] == '\0' || buf[len-1] == ' ')) --len;
    if (len == 0) return {};

    // Valid UTF-8: return as-is
    if (g_utf8_validate(buf, static_cast<gssize>(len), nullptr))
        return std::string(buf, len);

    // Try Shift-JIS → UTF-8 (Japanese region discs)
    gsize bytes_read = 0, bytes_written = 0;
    GError* err = nullptr;
    if (gchar* u = g_convert(buf, static_cast<gssize>(len),
                              "UTF-8", "SHIFT-JIS",
                              &bytes_read, &bytes_written, &err)) {
        std::string r(u, bytes_written);
        g_free(u);
        return r;
    }
    if (err) g_error_free(err);

    // Last resort: keep ASCII, replace non-ASCII bytes with '?'
    std::string r;
    r.reserve(len);
    for (std::size_t i = 0; i < len; ++i)
        r += (static_cast<unsigned char>(buf[i]) < 0x80) ? buf[i] : '?';
    return r;
}

std::string format_from_ext(const std::filesystem::path& p) {
    auto ext = p.extension().string();
    if (ext.size() > 1) ext = ext.substr(1);
    for (auto& c : ext) c = char(std::tolower(c));
    return ext;
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Find and cut a disc marker ("Disc 2", "(Disc 1 of 2)", "CD1") out of a
// name. Returns the disc number, or 0 if there is none.
int take_disc_number(std::string& name) {
    const std::string low = lower(name);
    for (const char* word : {"disc", "disk", "cd"}) {
        const std::size_t len = std::strlen(word);
        for (std::size_t pos = low.find(word); pos != std::string::npos;
             pos = low.find(word, pos + 1)) {
            // Must start a word, and be followed by a number.
            if (pos > 0 && std::isalnum(static_cast<unsigned char>(low[pos - 1])))
                continue;
            std::size_t i = pos + len;
            while (i < low.size() && (low[i] == ' ' || low[i] == '_' || low[i] == '-')) ++i;
            if (i >= low.size() || !std::isdigit(static_cast<unsigned char>(low[i])))
                continue;
            int number = 0;
            while (i < low.size() && std::isdigit(static_cast<unsigned char>(low[i])))
                number = number * 10 + (low[i++] - '0');
            // Swallow " of N" and the surrounding brackets.
            std::size_t j = i;
            while (j < low.size() && low[j] == ' ') ++j;
            if (low.compare(j, 2, "of") == 0) {
                j += 2;
                while (j < low.size() && (low[j] == ' ' || std::isdigit(static_cast<unsigned char>(low[j])))) ++j;
                i = j;
            }
            std::size_t start = pos;
            if (start > 0 && (name[start - 1] == '(' || name[start - 1] == '[')) {
                --start;
                if (i < name.size() && (name[i] == ')' || name[i] == ']')) ++i;
            }
            name.erase(start, i - start);
            return number;
        }
    }
    return 0;
}

struct ParsedName {
    std::string           title;     // cleaned, without the disc marker
    int                   disc = 0;
    std::filesystem::path set_dir;   // folder shared by the discs of a set
};

// Display title from the file name, the way a human would write it:
// "[GDI] Crazy Taxi 2 (US)" / "Jojo's ... v1.001 (2000)(Capcom)(NTSC)(US)[!]"
// become "Crazy Taxi 2" / "Jojo's ...". Dumps named generically (disc.gdi,
// track01.bin based sets) take the name of the folder they sit in, and
// "Game/Disc 2/disc.gdi" is disc 2 of "Game".
ParsedName parse_name(const std::filesystem::path& p) {
    ParsedName out;
    std::string s = p.stem().string();
    out.set_dir = p.parent_path();
    if (GameLibrary::is_generic_name(s)) {
        const auto parent = p.parent_path();
        s = parent.filename().string();
        std::string probe = s;
        const int folder_disc = take_disc_number(probe);
        // A folder that is nothing but "Disc 2": the game is one level up.
        bool only_marker = true;
        for (char c : probe)
            if (std::isalnum(static_cast<unsigned char>(c))) { only_marker = false; break; }
        if (folder_disc > 0 && only_marker && parent.has_parent_path()) {
            out.disc    = folder_disc;
            out.set_dir = parent.parent_path();
            s = parent.parent_path().filename().string();
        }
    }
    if (out.disc == 0)
        out.disc = take_disc_number(s);

    std::string text;
    int depth = 0;
    for (char c : s) {
        if (c == '(' || c == '[') { ++depth; continue; }
        if (c == ')' || c == ']') { if (depth > 0) --depth; continue; }
        if (depth == 0) text += (c == '_') ? ' ' : c;
    }
    // Drop a trailing dump version such as " v1.001".
    if (const auto v = text.rfind(" v"); v != std::string::npos && v + 2 < text.size() &&
        std::isdigit(static_cast<unsigned char>(text[v + 2])))
        text.erase(v);
    // Collapse whitespace.
    std::string tidy;
    for (char c : text)
        if (c != ' ' || (!tidy.empty() && tidy.back() != ' ')) tidy += c;
    while (!tidy.empty() && (tidy.back() == ' ' || tidy.back() == '-')) tidy.pop_back();
    out.title = tidy.empty() ? s : tidy;
    return out;
}
} // namespace

void GameLibrary::scan(const std::vector<std::filesystem::path>& dirs,
                       const std::vector<std::filesystem::path>& excluded) {
    namespace fs = std::filesystem;
    const auto is_excluded = [&](const fs::path& p) {
        return std::find(excluded.begin(), excluded.end(), p) != excluded.end();
    };
    m_games.clear();
    std::vector<std::string> incomplete;
    for (const auto& dir : dirs) {
        if (!std::filesystem::is_directory(dir)) continue;
        try {
            // Linked folders are followed (games kept on another drive), each
            // real folder once, so a link pointing back up cannot loop.
            std::set<fs::path> visited{fs::weakly_canonical(dir)};
            for (auto it = fs::recursive_directory_iterator(
                     dir, fs::directory_options::skip_permission_denied |
                          fs::directory_options::follow_directory_symlink);
                 it != fs::recursive_directory_iterator(); ++it) {
                const auto& e = *it;
                if (is_excluded(e.path()))
                    continue;
                if (e.is_directory()) {
                    std::error_code cec;
                    if (!visited.insert(fs::weakly_canonical(e.path(), cec)).second) {
                        it.disable_recursion_pending();
                        continue;
                    }
                }
                // An unzipped MAME set: a folder named after the set.
                if (e.is_directory()) {
                    const std::string name = e.path().filename().string();
                    const auto set = ds::engine::arcade_set(name);
                    if (!set)
                        continue;
                    it.disable_recursion_pending();
                    if (const auto missing = missing_from_set_folder(e.path(), *set); !missing.empty()) {
                        incomplete.push_back(name + " (no " + missing + ")");
                        continue;
                    }
                    GameEntry g;
                    g.path       = e.path();
                    g.root       = dir;
                    g.format     = set->gdrom.empty() ? "folder" : "folder + gd";
                    g.system     = set->board;
                    g.title      = set->title;
                    g.product_id = name;
                    g.size       = folder_size(e.path());
                    m_games.push_back(std::move(g));
                    continue;
                }
                if (!e.is_regular_file())
                    continue;
                if (has_disc_ext(e.path())) {
                    // A .cue beside a .gdi describes the same disc: list it once.
                    if (lower_ext(e.path()) == ".cue") {
                        bool has_gdi = false;
                        std::error_code ec;
                        for (const auto& sib : fs::directory_iterator(e.path().parent_path(), ec))
                            if (lower_ext(sib.path()) == ".gdi") { has_gdi = true; break; }
                        if (has_gdi) continue;
                    }
                    m_games.push_back(entry_from_path(e.path(), dir));
                } else if (is_arcade_set(e.path())) {
                    const auto set = ds::engine::arcade_set(e.path().stem().string());
                    GameEntry g;
                    g.path   = e.path();
                    g.root   = dir;
                    g.format = format_from_ext(e.path());
                    g.system = set->board;
                    g.title  = set->title;
                    g.product_id = e.path().stem().string();
                    g.size   = size_on_disk(e.path());
                    m_games.push_back(std::move(g));
                } else if (is_naomi_cart(e.path())) {
                    GameEntry g;
                    g.path   = e.path();
                    g.root   = dir;
                    g.format = format_from_ext(e.path());
                    g.system = "Naomi";
                    g.title  = title_from_file_name(e.path().stem().string());
                    g.size   = size_on_disk(e.path());
                    m_games.push_back(std::move(g));
                }
            }
        } catch (const std::filesystem::filesystem_error& ex) {
            ds::common::log()->warn("GameLibrary: scan error in {}: {}",
                                    dir.string(), ex.what());
        }
    }
    if (!incomplete.empty()) {
        std::string list;
        for (const auto& name : incomplete)
            list += (list.empty() ? "" : ", ") + name;
        ds::common::log()->info("GameLibrary: {} arcade set folder(s) left out, files missing: {}",
                                incomplete.size(), list);
    }
    // "Disc 1" with no other disc around is just a game.
    std::map<std::string, int> set_sizes;
    for (const auto& g : m_games)
        if (!g.set_key.empty()) ++set_sizes[g.set_key];
    for (auto& g : m_games) {
        if (!g.set_key.empty() && set_sizes[g.set_key] < 2) {
            g.set_key.clear();
            g.title = g.set_title;
            g.disc  = 0;
        }
    }
    std::sort(m_games.begin(), m_games.end(),
        [](const GameEntry& a, const GameEntry& b){ return lower(a.title) < lower(b.title); });
    ds::common::log()->info("GameLibrary: found {} game(s)", m_games.size());
    signal_updated.emit();
}

std::vector<const GameEntry*> GameLibrary::set_members(const GameEntry& game) const {
    std::vector<const GameEntry*> out;
    if (game.set_key.empty()) {
        out.push_back(&game);
        return out;
    }
    for (const auto& g : m_games)
        if (g.set_key == game.set_key)
            out.push_back(&g);
    std::sort(out.begin(), out.end(),
              [](const GameEntry* a, const GameEntry* b) { return a->disc < b->disc; });
    return out;
}

const GameEntry* GameLibrary::find(const std::filesystem::path& path) const {
    for (const auto& g : m_games)
        if (g.path == path)
            return &g;
    return nullptr;
}

bool GameLibrary::is_generic_name(const std::string& stem) {
    const std::string low = lower(stem);
    return low == "disc" || low == "disk" || low == "game" || low.rfind("track", 0) == 0;
}

GameEntry GameLibrary::entry_from_path(const std::filesystem::path& p,
                                       const std::filesystem::path& root) {
    GameEntry e;
    e.path   = p;
    e.root   = root;
    e.format = format_from_ext(p);
    e.system = "Dreamcast";
    e.size   = size_on_disk(p);

    const ParsedName parsed = parse_name(p);
    e.set_title = sanitize_to_utf8(parsed.title.c_str(), parsed.title.size());
    e.title     = e.set_title;
    e.disc      = parsed.disc;
    if (e.disc > 0) {
        e.title  += " (Disc " + std::to_string(e.disc) + ")";
        e.set_key = lower(e.set_title) + "|" + parsed.set_dir.string();
    }

    // Disc metadata from IP.BIN (sector 45000) where the image reader can
    // reach it. Layout: 0x30 area symbols, 0x40 product number, 0x80 title.
    if (auto disc_r = DiscImage::open(p)) {
        auto& disc = *disc_r;
        std::array<u8, 2048> sector{};
        if (disc->read_sector(kIpBinLba, sector) &&
            std::memcmp(sector.data(), "SEGA SEGAKATANA", 15) == 0) {
            const char* ip = reinterpret_cast<const char*>(sector.data());
            e.product_id = sanitize_to_utf8(ip + 0x40, 10);
            e.region     = sanitize_to_utf8(ip + 0x30, 8);
            e.disc_title = sanitize_to_utf8(ip + 0x80, 128);
        }
    }
    return e;
}

} // namespace ds::frontend
