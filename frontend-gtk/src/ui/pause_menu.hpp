// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gtkmm.h>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ds::frontend {

// The in-game pause menu, laid over the paused game image. Follows
// DuckStation's: game title and play time, then Resume, Fast Forward,
// Load State, Save State, Game Properties, Screenshot and Close Game (with
// its own sub-menu). Load/Save open a grid of slots with screenshots.
// Works with keyboard, mouse and the controller on port A (D-pad / stick to
// move, A to choose, B to go back).
class PauseMenu : public Gtk::Box {
public:
    struct Slot {
        bool        exists = false;
        std::string when;                  // "2026-10-12 05:21"
        std::filesystem::path screenshot;  // may not exist
    };
    struct Info {
        std::string title;
        std::string subtitle;              // product number or file name
        std::string session_time;
        std::string total_time;
        bool        can_save_states = true;
        bool        fast_forward    = false;
        std::function<Slot(bool global, int slot)> slot;
        // Other discs that can be swapped in: label, path.
        std::vector<std::pair<std::string, std::string>> discs;
    };

    PauseMenu();
    ~PauseMenu() override;

    void open(Info info);
    void close_menu();
    bool is_open() const { return get_visible(); }
    // Escape / B: step back one level; returns false when already at the top
    // (the caller then closes the menu).
    bool go_back();

    // "resume", "fast-forward", "screenshot", "properties", "restart",
    // "close-save", "close-nosave", "load:game:3", "save:global:1", ...
    sigc::signal<void(std::string)> signal_action;

private:
    void show_view(const std::string& name);
    void build_main();
    void build_close();
    void build_slots(bool save);
    void build_discs();
    Gtk::Button* add_button(Gtk::Box& box, const std::string& label, const std::string& action);
    bool poll_pad();
    void move_focus(int dx, int dy);

    Info       m_info;
    Gtk::Label m_title, m_subtitle, m_times, m_clock;
    Gtk::Stack m_stack;
    Gtk::Box   m_main{Gtk::Orientation::VERTICAL, 8};
    Gtk::Box   m_close{Gtk::Orientation::VERTICAL, 8};
    Gtk::Box   m_slots{Gtk::Orientation::VERTICAL, 12};
    Gtk::Button* m_ff_button = nullptr;

    // Focus order of the current view, and its column count (1 = list).
    std::vector<Gtk::Widget*> m_focus;
    int         m_columns = 1;
    std::size_t m_index   = 0;
    std::string m_view    = "main";

    sigc::connection m_pad_timer;
    unsigned         m_pad_prev = ~0u;   // swallow buttons held when opening
};

} // namespace ds::frontend
