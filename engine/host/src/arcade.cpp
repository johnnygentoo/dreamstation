// SPDX-License-Identifier: GPL-2.0-or-later
#include <dreamstation/engine/engine.hpp>
#include <fstream>
#include <vector>
#include <zlib.h>

// Implemented in the fork (shell/dreamstation/arcade_lookup.cpp), next to
// the engine's table of known Naomi / Naomi 2 / Atomiswave sets.
extern "C" const char* dreamstation_arcade_title(const char* set_name);
extern "C" const char* dreamstation_arcade_board(const char* set_name);
extern "C" const char* dreamstation_arcade_parent(const char* set_name);
extern "C" const char* dreamstation_arcade_gdrom(const char* set_name);
extern "C" bool dreamstation_arcade_rom(const char* set_name, int index, const char** file_name,
                                        unsigned* length, unsigned* crc, bool* optional);

namespace ds::engine {

std::string arcade_title(const std::string& set_name)
{
    const char* title = dreamstation_arcade_title(set_name.c_str());
    return title ? title : std::string{};
}

std::optional<ArcadeSet> arcade_set(const std::string& set_name)
{
    const char* title = dreamstation_arcade_title(set_name.c_str());
    if (!title)
        return std::nullopt;
    const auto str = [](const char* s) { return s ? std::string(s) : std::string{}; };
    ArcadeSet set;
    set.title  = title;
    set.board  = str(dreamstation_arcade_board(set_name.c_str()));
    set.parent = str(dreamstation_arcade_parent(set_name.c_str()));
    set.gdrom  = str(dreamstation_arcade_gdrom(set_name.c_str()));
    const char* file = nullptr;
    unsigned length = 0, crc = 0;
    bool optional = false;
    for (int i = 0; dreamstation_arcade_rom(set_name.c_str(), i, &file, &length, &crc, &optional); ++i)
        set.roms.push_back({file, length, crc, optional});
    return set;
}

bool arcade_rom_matches(const std::filesystem::path& file, const ArcadeSet::Rom& rom)
{
    std::error_code ec;
    if (std::filesystem::file_size(file, ec) != rom.length || ec)
        return false;
    std::ifstream in(file, std::ios::binary);
    std::vector<char> buf(1 << 20);
    uLong crc = crc32(0, Z_NULL, 0);
    while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0)
        crc = crc32(crc, reinterpret_cast<const Bytef*>(buf.data()), static_cast<uInt>(in.gcount()));
    return static_cast<std::uint32_t>(crc) == rom.crc;
}

} // namespace ds::engine
