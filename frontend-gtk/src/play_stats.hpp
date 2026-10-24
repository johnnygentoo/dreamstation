// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ctime>
#include <map>
#include <string>

namespace ds::frontend {

// Time played and last played per game, kept in <data>/playtime.toml.
class PlayStats {
public:
    struct Entry {
        long long   seconds = 0;
        std::time_t last    = 0;
    };

    void load();
    void save() const;

    Entry get(const std::string& game_id) const;
    void  add_seconds(const std::string& game_id, long long seconds);
    void  touch(const std::string& game_id);       // set last played = now
    void  reset(const std::string& game_id);

    static std::string format_duration(long long seconds);   // "3h 12m", "45m", ""
    static std::string format_date(std::time_t t);            // "2026-10-12", "Never"

private:
    std::map<std::string, Entry> m_entries;
};

} // namespace ds::frontend
