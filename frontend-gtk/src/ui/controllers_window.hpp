// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <libadwaitamm.h>
#include <gtkmm.h>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../config.hpp"

namespace ds::frontend {

// The controller settings window, separate from Settings as in DuckStation:
// one page per controller port (device, type, bindings) and a Hotkeys page.
// Click a binding button, then press a key or a pad button/axis to bind it;
// Escape cancels, Backspace or Delete clears.
class ControllersWindow : public Gtk::Window {
public:
    explicit ControllersWindow(Config& config);
    ~ControllersWindow() override;

    void show_page(const std::string& id);   // "port0".."port3", "hotkeys"

    // Bindings, port types or hotkeys changed and were saved to the config.
    sigc::signal<void()> signal_changed;

private:
    struct Capture {
        Gtk::Button*                               button = nullptr;
        enum class Mode { Any, Keys, PadButtons };
        Mode                                       mode = Mode::Any;
        std::function<void(const std::string&)>    apply;     // "" = clear
        std::function<std::string()>               current;   // label to restore
    };

    Gtk::Box&     add_page(const std::string& id, const std::string& title, const char* icon);
    Gtk::ListBox& add_group(Gtk::Box& page, const std::string& title);
    void build_port_page(unsigned port);
    void build_hotkeys_page();
    void begin_capture(Capture capture, unsigned port);
    void end_capture(bool restore_label);
    bool on_capture_key(guint keyval);
    void refresh_device_labels();
    void refresh_game_inputs();

    Config& m_config;
    Gtk::Box     m_root{Gtk::Orientation::VERTICAL};
    Gtk::Box     m_body{Gtk::Orientation::HORIZONTAL};
    Gtk::ListBox m_sidebar;
    Gtk::Stack   m_stack;
    Gtk::Label   m_hint;
    std::vector<std::string> m_page_ids;
    std::vector<Gtk::Label*> m_device_labels;
    std::vector<std::function<void()>> m_refreshers;   // re-read binding labels
    // Each controller input's row, for what it does in the running game.
    std::vector<std::pair<Adw::ActionRow*, std::string>> m_input_rows;

    Capture          m_capture;
    bool             m_capturing = false;
    sigc::connection m_capture_timer;
    sigc::connection m_device_timer;
};

} // namespace ds::frontend
