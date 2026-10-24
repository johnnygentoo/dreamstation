// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gtkmm.h>
#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace ds::frontend {

// The on-screen display drawn over the game: messages, a performance
// read-out and the save-slot selector. Purely informational — it never
// takes input, so clicks and keys go straight through to the game view.
class OsdLayer : public Gtk::Box {
public:
    OsdLayer();
    ~OsdLayer() override;

    // A message that fades after `seconds`. Repeating the newest message's
    // text refreshes it instead of stacking a duplicate.
    void message(const std::string& text, double seconds = 3.0);
    void clear_messages();

    // Lines of the performance read-out (top right); empty hides it.
    void set_performance(const std::vector<std::string>& lines);

    struct SlotInfo {
        std::string title;                 // "Game Slot 3"
        std::string detail;                // "Saved at 21:07 on 2026-10-12" / "No save present..."
        std::string legend;                // key hints
        std::filesystem::path screenshot;  // may not exist
    };
    // The slot selector (bottom centre), shown for a few seconds.
    void show_slot(const SlotInfo& info);

    // A VMU's screen (48x32 dots, non-zero = lit) in a bottom corner, or
    // none (null).
    void set_vmu_screen(const std::uint8_t* dots);

    // corner: top-left | top-center | top-right | bottom-left | bottom-center | bottom-right
    void set_message_corner(const std::string& corner);
    void set_scale(int percent);

private:
    struct Message {
        Gtk::Label* label;
        gint64      expires_us;
    };
    bool tick();
    void ensure_timer();

    Gtk::Box     m_messages{Gtk::Orientation::VERTICAL, 6};
    Gtk::Box     m_perf{Gtk::Orientation::VERTICAL, 7};
    Gtk::Box     m_slot{Gtk::Orientation::VERTICAL, 6};
    Gtk::Picture m_slot_picture;
    Gtk::Label   m_slot_title, m_slot_detail, m_slot_legend;
    Gtk::CenterBox m_top, m_bottom;
    Gtk::DrawingArea m_vmu;
    std::array<std::uint8_t, 48 * 32> m_vmu_dots{};
    std::deque<Message> m_active;
    gint64           m_slot_expires_us = 0;
    sigc::connection m_timer;
    Glib::RefPtr<Gtk::CssProvider> m_scale_css;
    std::string m_corner = "top-left";
};

} // namespace ds::frontend
