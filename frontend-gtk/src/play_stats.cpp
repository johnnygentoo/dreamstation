// SPDX-License-Identifier: GPL-3.0-or-later
#include "play_stats.hpp"
#include "config.hpp"
#include <toml++/toml.hpp>
#include <fstream>

namespace ds::frontend {

void PlayStats::load() {
    m_entries.clear();
    const auto path = Config::data_dir() / "playtime.toml";
    if (!std::filesystem::exists(path))
        return;
    try {
        const auto t = toml::parse_file(path.string());
        for (const auto& [id, node] : t) {
            const auto* tbl = node.as_table();
            if (!tbl) continue;
            Entry e;
            if (auto v = (*tbl)["seconds"].value<int64_t>()) e.seconds = *v;
            if (auto v = (*tbl)["last"].value<int64_t>())    e.last = static_cast<std::time_t>(*v);
            m_entries[std::string(id.str())] = e;
        }
    } catch (const toml::parse_error&) {
        // A damaged stats file is not worth failing over; start again.
    }
}

void PlayStats::save() const {
    auto root = toml::table{};
    for (const auto& [id, e] : m_entries)
        root.emplace(id, toml::table{{"seconds", static_cast<int64_t>(e.seconds)},
                                     {"last", static_cast<int64_t>(e.last)}});
    std::error_code ec;
    std::filesystem::create_directories(Config::data_dir(), ec);
    std::ofstream f(Config::data_dir() / "playtime.toml");
    if (f) f << root;
}

PlayStats::Entry PlayStats::get(const std::string& game_id) const {
    const auto it = m_entries.find(game_id);
    return it == m_entries.end() ? Entry{} : it->second;
}

void PlayStats::add_seconds(const std::string& game_id, long long seconds) {
    if (!game_id.empty())
        m_entries[game_id].seconds += seconds;
}

void PlayStats::touch(const std::string& game_id) {
    if (!game_id.empty())
        m_entries[game_id].last = std::time(nullptr);
}

void PlayStats::reset(const std::string& game_id) { m_entries.erase(game_id); }

std::string PlayStats::format_duration(long long seconds) {
    if (seconds <= 0)
        return {};
    const long long h = seconds / 3600, m = (seconds % 3600) / 60;
    if (h > 0)
        return std::to_string(h) + "h " + std::to_string(m) + "m";
    if (m > 0)
        return std::to_string(m) + "m";
    return std::to_string(seconds) + "s";
}

std::string PlayStats::format_date(std::time_t t) {
    if (t == 0)
        return "Never";
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::localtime(&t));
    return buf;
}

} // namespace ds::frontend
