// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <libadwaitamm.h>
#include <gtkmm.h>
#include <sigc++/sigc++.h>
#include <dreamstation/engine/engine.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../config.hpp"

namespace ds::frontend {

// The settings window, in DuckStation's shape: a category list on the left,
// the selected page on the right, and a help panel at the bottom describing
// whatever control the pointer is over. Changes apply immediately.
//
// Engine pages are generated from the engine's own option list, so every
// option the engine has is reachable. Constructed with a GameContext, the
// same window edits that game's overrides instead: every option gains a
// "Use Global Setting" choice and the frontend-only pages are replaced by a
// Summary page.
class SettingsWindow : public Gtk::Window {
public:
    struct GameContext {
        std::string id;                 // names the per-game settings file
        std::string title;
        std::filesystem::path path;
        std::string product_id;
        std::string region;
        std::string disc_title;
    };

    SettingsWindow(Config& config, std::vector<ds::engine::OptionInfo> options,
                   std::optional<GameContext> game = std::nullopt);

    // Navigate to a page by id ("interface", "graphics", "bios", ...).
    void show_page(const Glib::ustring& id);

    bool per_game() const { return m_game.has_value(); }
    const std::string& game_id() const { return m_game_id; }

    // A frontend setting changed (global mode only).
    sigc::signal<void()> signal_changed;
    // An engine option changed in this layer. MainWindow recomputes the
    // effective value (default < global < per-game) and applies it.
    sigc::signal<void(std::string key)> signal_engine_option;
    // The post-processing chain changed shape (stages added, removed,
    // reordered, toggled) / one option of one stage changed.
    sigc::signal<void()> signal_post_chain;
    sigc::signal<void(std::size_t stage, std::string name, float value)> signal_post_option;
    // The user asked to wipe this layer; the owner should reopen the window.
    sigc::signal<void()> signal_reset;

private:
    struct Page {
        Gtk::Box* box = nullptr;        // vertical container of groups
    };

    Page          add_page(const std::string& id, const std::string& title, const char* icon);
    Gtk::ListBox& add_group(Page& page, const std::string& title, const std::string& description = {});
    Adw::ActionRow* add_row(Gtk::ListBox& group, const std::string& title,
                            const std::string& help, Gtk::Widget* suffix);
    void add_switch(Gtk::ListBox& group, const std::string& title, const std::string& help,
                    bool& value);
    void add_engine_option(Gtk::ListBox& group, const ds::engine::OptionInfo& opt);
    void add_engine_options(Page& page, const std::string& group_title,
                            const std::vector<std::string>& keys);
    void add_engine_category(Page& page, const std::string& group_title,
                             const std::string& category);
    void add_path_row(Gtk::ListBox& group, const std::string& title, const std::string& help,
                      std::filesystem::path& target, const char* pattern);
    void rebuild_game_dirs();
    void rebuild_post_page();
    void open_shader_picker();
    void set_help(const std::string& title, const std::string& text);

    void build_summary_page();
    void build_interface_page();
    void build_game_list_page();
    void build_bios_page();
    void build_console_page();
    void build_online_page();
    void build_emulation_page();
    void build_vmu_page();
    void build_graphics_page();
    void build_post_processing_page();
    void build_audio_page();
    void build_osd_page();
    void build_controllers_page();
    void build_advanced_page();

    // This layer's stored value for an engine key (nullopt = not set here).
    std::optional<std::string> layer_value(const std::string& key) const;
    void set_layer_value(const std::string& key, const std::optional<std::string>& value);
    // What applies underneath this layer (global value, else engine default).
    std::string inherited_value(const ds::engine::OptionInfo& opt) const;

    Config&                              m_config;
    std::vector<ds::engine::OptionInfo>  m_options;
    std::optional<GameContext>           m_game;
    std::string                          m_game_id;
    GameSettings                         m_game_settings;
    std::map<std::string, bool>          m_placed;      // engine keys already shown

    Gtk::Box      m_root{Gtk::Orientation::VERTICAL};
    Gtk::Box      m_body{Gtk::Orientation::HORIZONTAL};
    Gtk::ListBox  m_sidebar;
    Gtk::Stack    m_stack;
    Gtk::Label    m_help_title;
    Gtk::Label    m_help_text;
    Gtk::ListBox* m_game_dirs = nullptr;
    Gtk::ListBox* m_excluded  = nullptr;
    Gtk::Box*     m_post_box  = nullptr;
    std::vector<std::string> m_page_ids;
};

} // namespace ds::frontend
