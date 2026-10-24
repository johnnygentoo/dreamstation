// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <sigc++/sigc++.h>

namespace ds::frontend {

struct GameEntry {
    std::string title;       // display title, derived from the file/folder name
    std::string set_title;   // title without the disc marker ("Skies of Arcadia")
    std::string set_key;     // identifies a multi-disc set; empty for single discs
    int         disc = 0;    // disc number within a set, 0 if not part of one
    std::string disc_title;  // title stored on the disc (IP.BIN), may be empty
    std::string product_id;  // e.g. "MK-51000"
    std::string region;      // "J", "U", "E", or multi-region
    std::string format;      // "gdi", "cdi", "chd", "cue", "zip", ...
    std::string system;      // "Dreamcast", "Naomi", "Naomi 2", "Atomiswave" or "System SP"
    std::uintmax_t size = 0; // bytes on disk
    std::filesystem::path path;
    std::filesystem::path root;  // the game directory this entry was found under

    bool is_arcade() const { return system != "Dreamcast"; }
};

class GameLibrary {
public:
    // Synchronously scan the given directories and update the game list.
    // Emits signal_updated when done (on the calling thread).
    void scan(const std::vector<std::filesystem::path>& dirs,
              const std::vector<std::filesystem::path>& excluded = {});

    const std::vector<GameEntry>& games() const { return m_games; }
    bool empty() const { return m_games.empty(); }

    // The other discs of the same game (including `game`), ordered by disc
    // number. Just `game` itself when it is not part of a set.
    std::vector<const GameEntry*> set_members(const GameEntry& game) const;
    const GameEntry* find(const std::filesystem::path& path) const;

    // True for dump file names that say nothing about the game (disc.gdi).
    static bool is_generic_name(const std::string& stem);

    // Fires after every scan() call, even if the list is empty.
    sigc::signal<void()> signal_updated;

private:
    static GameEntry entry_from_path(const std::filesystem::path& p,
                                     const std::filesystem::path& root);

    std::vector<GameEntry> m_games;
};

} // namespace ds::frontend
