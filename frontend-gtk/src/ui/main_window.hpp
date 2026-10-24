// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <libadwaitamm.h>
#include <gtkmm.h>
#include <dreamstation/common/types.hpp>
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "../config.hpp"
#include "../game_library.hpp"
#include "../hotkeys.hpp"
#include "../play_stats.hpp"
#include <dreamstation/engine/engine.hpp>
#include <functional>
#include <vector>

namespace ds::frontend {

class LibraryPage;
class RenderWidget;
class SettingsWindow;
class CoverDownloader;
class ControllersWindow;
class PauseMenu;
class VmuManager;
class VmuPlayer;
class VmuWindow;
class NetplayDialog;
class OsdLayer;

class MainWindow : public Adw::ApplicationWindow {
public:
    explicit MainWindow(Config& config);
    ~MainWindow() override;

    // Boot a game by path: asks before replacing a running game and offers
    // the resume state when one exists.
    void launch(const std::filesystem::path& game, bool pick_disc = true);

    // Trigger a rescan of the game library (e.g. after config changes).
    void rescan_library();
    void ensure_cover_downloader();
    void fetch_missing_covers();
    void watch_game_dirs();
    void on_game_dir_changed();
    // VMUs during a game: their screens (overlay, VMU window) and mini-games
    // a game puts on them.
    void ensure_vmu_window();
    void set_vmu_window_visible(bool visible);
    std::filesystem::path vmu_card_path(int port) const;
    bool take_vmu_out(int port, bool out);   // false if refused
    bool poll_vmus();
    std::string vmu_game_fingerprint(int port, std::string* title = nullptr) const;

private:
    // ── Build ─────────────────────────────────────────────────────────────
    void setup_actions();
    void build_menu();
    void build_ui();
    void update_sensitivity(); // enable/disable actions based on emu state

    // ── Action handlers ───────────────────────────────────────────────────
    void on_open_disc();
    void on_start_bios();
    void on_pause_toggle(bool paused);
    void on_reset();
    void on_power_off();
    void on_fullscreen_toggle(bool fs);
    void on_show_toolbar(bool v);
    void on_show_status_bar(bool v);
    void on_search_toggle(bool v);
    void apply_post_chain();
    void on_resolution_changed(u32 scale);
    void on_widescreen_changed(const std::string& mode);
    std::string widescreen_mode() const;
    void toggle_fullscreen();
    void apply_chrome_visibility();
    void on_open_data_dir();
    void on_about();
    void open_settings(const std::string& page = "interface");
    void open_game_properties(const std::filesystem::path& game);
    void connect_settings(SettingsWindow& win);
    void apply_engine_option(const std::string& key);
    std::vector<ds::engine::OptionInfo> engine_options();

    // ── Emulation lifecycle ───────────────────────────────────────────────
    void start_emulation(const std::filesystem::path& disc, bool disc_less = false,
                         const std::optional<ds::engine::Engine::Netplay>& netplay = std::nullopt);
    // Online play with a friend (GGPO netplay).
    void open_netplay_dialog(const std::filesystem::path& game);
    bool prepare_netplay_dirs(std::filesystem::path& system_dir, std::filesystem::path& save_dir,
                              std::string& error);
    bool poll_netplay();
    // During online play: refuses what would put the two sides out of step,
    // saying why when `what` is given.
    bool netplay_blocks(const char* what = nullptr);
    void stop_emulation();
    bool prepare_system_dir(std::filesystem::path& system_dir, std::string& error);
    std::string internal_resolution() const;

    // ── Save states (main_window_states.cpp) ──────────────────────────────
    static constexpr int kSlots = 10;
    static std::string game_id_for(const std::filesystem::path& game);
    std::filesystem::path slot_path(bool global, int slot) const;
    std::filesystem::path resume_path(const std::string& game_id) const;
    bool save_state_file(const std::filesystem::path& file);
    bool load_state_file(const std::filesystem::path& file);
    void save_slot(bool global, int slot);
    void load_slot(bool global, int slot);
    void on_state_action(const std::string& target, bool save);
    void undo_load_state();
    void select_slot(int delta);
    void rebuild_state_menus();
    void take_screenshot();

    // ── Window behaviour (main_window_states.cpp) ─────────────────────────
    void set_paused(bool paused);
    // Close the running game, asking first if configured. `after` runs once
    // the game has actually been closed.
    void request_close_game(std::function<void()> after = {}, bool allow_save = true);
    void close_game_now(bool save_resume);
    void handle_hotkey(const std::string& id, bool pressed);
    void update_cursor_and_inhibit();
    void on_game_action(const std::string& action, const std::filesystem::path& game);
    void flush_play_time();
    void open_controllers(const std::string& page);
    void apply_input_config();
    void open_pause_menu();
    void close_pause_menu(bool resume);
    void on_pause_menu_action(const std::string& action);
    void change_disc(const std::string& target);      // path, "file" or "remove"
    void rebuild_disc_menu();
    void apply_audio_config();
    void apply_display_config();
    void apply_osd_config();
    void show_slot_osd();
    bool poll_pad_hotkeys();
    void run_test_script();
    void save_window_image(const std::string& file, Gtk::Window* window = nullptr);

    // ── UI helpers ────────────────────────────────────────────────────────
    void show_toast(const Glib::ustring& msg);
    void update_fps_display();
    void set_game_visible(bool v);

    // ── Config ────────────────────────────────────────────────────────────
    Config& m_config;

    // ── Game library ──────────────────────────────────────────────────────
    GameLibrary m_library;

    // ── Layout ────────────────────────────────────────────────────────────
    Adw::ToastOverlay              m_toasts;
    Gtk::Box                       m_root{Gtk::Orientation::VERTICAL};
    Adw::HeaderBar                 m_header;
    Adw::ViewSwitcherTitle         m_view_title;
    Gtk::ActionBar                 m_toolbar;
    Adw::ViewStack                 m_views;
    Gtk::Box                       m_status_bar{Gtk::Orientation::HORIZONTAL};

    std::unique_ptr<LibraryPage>   m_library_page;
    std::unique_ptr<RenderWidget>  m_render_widget;
    std::unique_ptr<SettingsWindow> m_settings_win;
    std::unique_ptr<SettingsWindow> m_game_settings_win;
    std::unique_ptr<CoverDownloader> m_cover_downloader;
    std::unique_ptr<ControllersWindow> m_controllers_win;
    std::unique_ptr<VmuManager> m_vmu_manager;
    std::unique_ptr<VmuWindow>  m_vmu_window;
    std::unique_ptr<NetplayDialog> m_netplay_dialog;
    bool             m_netplay = false;          // the running game is online
    bool             m_netplay_waiting = false;  // still connecting
    bool             m_netplay_host = false;     // this side hosts (player 1)
    sigc::connection m_netplay_timer;
    std::unique_ptr<VmuPlayer>  m_vmu_player;
    // Plays the mini-game on a card image, or in a .vms / .dci file.
    void play_vmu_game(const std::filesystem::path& source);

    // Game view: render surface with an on-screen-display overlay (FPS).
    Gtk::Overlay m_game_overlay;
    Gtk::Label   m_osd_fps;
    std::unique_ptr<OsdLayer> m_osd;
    std::unique_ptr<PauseMenu> m_pause_menu;
    bool         m_menu_paused_game = false;

    // Status bar labels
    Gtk::Label  m_status_game_lbl;
    Gtk::Label  m_status_fps_lbl;
    Gtk::Label  m_status_scale_lbl;

    // ── Stateful actions (kept for state updates) ─────────────────────────
    Glib::RefPtr<Gio::SimpleAction> m_act_pause;
    Glib::RefPtr<Gio::SimpleAction> m_act_fullscreen;
    Glib::RefPtr<Gio::SimpleAction> m_act_toolbar;
    Glib::RefPtr<Gio::SimpleAction> m_act_statusbar;
    Glib::RefPtr<Gio::SimpleAction> m_act_search;
    Glib::RefPtr<Gio::SimpleAction> m_act_post;     // bool: post-processing chain on/off
    Glib::RefPtr<Gio::SimpleAction> m_act_resolution; // radio: internal resolution scale (0 = automatic)
    Glib::RefPtr<Gio::SimpleAction> m_act_widescreen; // radio: off | hack | cheats
    sigc::connection m_auto_res_timer;
    std::string      m_applied_resolution;

    // ── Emulation state (the engine session lives in RenderWidget) ────────
    bool        m_emu_running = false;
    std::string m_current_game_title;
    std::filesystem::path m_current_game_path;
    std::string m_game_id;                 // names this game's state files

    // ── Save states / behaviour ───────────────────────────────────────────
    HotkeyMap   m_hotkeys;
    PlayStats   m_stats;
    long long   m_session_seconds = 0;
    int         m_selected_slot   = 1;
    bool        m_selected_global = false;
    std::vector<char> m_undo_state;        // state before the last load
    bool        m_paused_by_view  = false;
    bool        m_paused_by_focus = false;
    bool        m_closing         = false;
    guint       m_inhibit_cookie  = 0;
    Glib::RefPtr<Gio::SimpleActionGroup> m_action_group;
    Glib::RefPtr<Gio::Menu> m_load_menu;
    Glib::RefPtr<Gio::Menu> m_save_menu;
    Glib::RefPtr<Gio::Menu> m_disc_menu;
    Glib::RefPtr<Gio::SimpleAction> m_act_merge;
    Glib::RefPtr<Gio::SimpleAction> m_act_box_covers;
    Glib::RefPtr<Gio::SimpleAction> m_act_auto_covers;
    sigc::connection                m_cover_refresh;   // pending list refresh for new covers
    std::int64_t                    m_test_start_ms = 0;   // scripted test runs: when the script began
    // Game folders being watched, and the rescan waiting for them to settle
    // (or for the running game to close).
    std::vector<Glib::RefPtr<Gio::FileMonitor>> m_dir_monitors;
    Glib::RefPtr<Gio::SimpleAction> m_act_vmu_overlay, m_act_vmu_window;
    sigc::connection m_vmu_timer;
    std::array<std::uint64_t, 4> m_vmu_serial{};   // last screen shown, per port
    // Per port: the card's mini-game as last seen, to notice a game putting
    // a new one on it; changes are read once the card has been quiet.
    struct VmuWatch {
        bool known = false;
        std::string fingerprint;
        std::filesystem::file_time_type time{};
        int quiet = -1;                  // polls since the card changed; -1 = settled
    };
    std::array<VmuWatch, 4> m_vmu_watch;
    int  m_vmu_ticks = 0;
    bool m_paused_by_vmu = false;
    sigc::connection                m_rescan_timer;
    bool                            m_rescan_after_game = false;
    sigc::connection m_pad_hotkey_timer;
    unsigned         m_pad_buttons_prev = ~0u;
    Gtk::Label  m_status_video_lbl;

    // ── FPS tracking ──────────────────────────────────────────────────────
    sigc::connection m_fps_timer;
    u64              m_last_presented = 0;
    u64              m_last_emulated  = 0;
};

} // namespace ds::frontend
