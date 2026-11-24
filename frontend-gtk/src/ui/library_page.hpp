// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <libadwaitamm.h>
#include <gtkmm.h>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../game_library.hpp"
#include "../play_stats.hpp"
#include "cover_cache.hpp"

namespace ds::frontend {

struct Config;

// Where cover art may be found, and the cover for one game (empty if none).
std::vector<std::filesystem::path> cover_dirs();
std::filesystem::path find_cover(const GameEntry& game,
                                 const std::vector<std::filesystem::path>& dirs);
// The art the game grid shows for a game, empty if none. With `box`, a
// picture of the whole box in 3D (ES-DE's "3dboxes") where there is one,
// else the flat cover.
std::filesystem::path find_art(const GameEntry& game, bool box);
// Index of the cover name (a file name without extension) that best fits a
// game's title, or -1. Spelling, punctuation and bracketed notes such as
// "(Rev A)" or "(USA)" are ignored, as covers from different sources name
// games differently. Among equally good names, one for the game's region
// ("J", "U", "E" letters) and disc number is preferred.
int match_cover_name(const std::string& title, const std::vector<std::string>& names,
                     const std::string& region = {}, int disc = 0);
// The title to look a game's cover up by: without the disc for multi-disc
// games, with the other name an arcade game is known by as an alternative.
std::string cover_title(const GameEntry& game);
// Covers in other programs' folders are indexed once; this makes the next
// lookup index them again (they may have downloaded more).
void forget_cover_index();

// The game list page. Like DuckStation's: a header strip (list/grid, type and
// region filters, sort, cover scale, search) over either a cover grid or a
// sortable table, with a right-click menu on every game.
class LibraryPage : public Gtk::Box {
public:
    LibraryPage(Config& config, PlayStats& stats,
                std::function<std::string(const std::filesystem::path&)> game_id_for);

    void set_library(GameLibrary* lib);
    // For scripted test runs: grid tiles showing a cover, and tiles in all.
    std::pair<int, int> covers_shown() const;
    void test_cover_size(int size) { m_scale.set_value(size); }
    void set_search_visible(bool v);
    bool search_visible() const;
    void set_view(const std::string& view);       // "grid" | "list"
    void zoom(int steps);                          // grid cover size
    void refresh();                                // rebuild from the library

    sigc::signal<void(std::string)> signal_launch_game;
    // Context-menu actions: "properties", "open-dir", "set-cover",
    // "fresh-boot", "delete-states", "exclude", "reset-playtime".
    sigc::signal<void(std::string action, std::string path)> signal_game_action;

private:
    void rebuild();
    void rebuild_grid(const std::vector<const GameEntry*>& games);
    void rebuild_list(const std::vector<const GameEntry*>& games);
    std::vector<const GameEntry*> visible_games() const;
    std::string display_title(const GameEntry& g) const;
    int         disc_count(const GameEntry& g) const;
    void show_context_menu(Gtk::Widget& anchor, double x, double y, const std::string& path);
    void build_header();
    void build_list_columns();

    Config&      m_config;
    PlayStats&   m_stats;
    std::function<std::string(const std::filesystem::path&)> m_game_id_for;
    GameLibrary* m_library = nullptr;

    // Header strip
    Gtk::Box          m_header{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::ToggleButton m_grid_btn;
    Gtk::ToggleButton m_list_btn;
    Gtk::DropDown     m_type_filter;
    Gtk::DropDown     m_region_filter;
    Gtk::DropDown     m_sort;
    Gtk::ToggleButton m_desc_btn;
    Gtk::Scale        m_scale{Gtk::Orientation::HORIZONTAL};
    sigc::connection  m_scale_settle;     // the size, once a drag stops
    Gtk::SearchEntry  m_search_entry;
    Gtk::Label        m_count;

    Gtk::Stack          m_inner_stack;
    Adw::StatusPage     m_empty_page;
    Gtk::ScrolledWindow m_grid_scroll;
    Gtk::FlowBox        m_flow;
    Gtk::ScrolledWindow m_list_scroll;
    Gtk::ColumnView     m_columns;
    Glib::RefPtr<Gio::ListStore<Glib::Object>> m_store;

    // Context menu
    Gtk::PopoverMenu                     m_menu;
    Glib::RefPtr<Gio::SimpleActionGroup> m_actions;
    std::string                          m_menu_path;

    // Covers arrive from the cache in the background; a tile only takes one
    // meant for the grid it belongs to.
    CoverCache m_covers;
    unsigned   m_grid_generation = 0;
    bool                     m_building = false;
    std::map<std::string, int> m_set_counts;   // discs per merged set
};

} // namespace ds::frontend
