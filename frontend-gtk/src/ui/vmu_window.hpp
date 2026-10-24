// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gtkmm.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vmu_unit.hpp"

namespace ds::frontend {

// The VMUs plugged into the controllers, in a window of their own (View >
// VMU Window). While a game runs each shows what the game draws on its
// screen. When a game puts a mini-game on a VMU (Chao Adventure from Sonic
// Adventure's Chao Garden, ...), it can be played here: the VMU is taken out
// of the controller for that time, as on the real console, and plugged back
// in afterwards so the game finds what the mini-game left on it.
class VmuWindow : public Gtk::Window {
public:
    struct Hooks {
        std::function<bool()> game_running;
        // The card file of the VMU in controller `port` (0..3), slot 1.
        std::function<std::filesystem::path(int port)> card_path;
        // Take the VMU out of (true) or put it back into the running game.
        // False if it may not be taken out now.
        std::function<bool(int port, bool out)> take_out;
    };
    VmuWindow(Hooks hooks, int volume);
    ~VmuWindow() override;

    using Dots = std::array<std::uint8_t, 48 * 32>;
    // A game's VMU screen for a port, or null when it has drawn none.
    void show_screen(int port, const Dots* dots);
    // Rebuilds the list of VMUs and what each holds (after a game starts or
    // stops, or a card changed).
    void refresh();
    // A game just put a mini-game on that port's VMU.
    void announce(int port, const std::string& game_title);
    // Ends any mini-game, saving it and plugging its VMU back in.
    void stop_all();

    // For scripted test runs.
    // Play Mini-Game / Plug Back In on that port.
    bool test_toggle(int port, std::string& error);
    void test_hold(unsigned buttons);

private:
    struct Slot {
        int         port = 0;
        Gtk::Box    box{Gtk::Orientation::VERTICAL, 6};
        Gtk::Label  title;
        std::unique_ptr<VmuUnit> unit;
        Gtk::Button play;
        bool        out = false;     // taken out of the running game
        bool        has_game = false;
    };
    Slot* slot_for(int port);
    void update_slot(Slot& slot);
    void toggle_play(Slot& slot);
    bool start_play(Slot& slot, std::string& error);
    void end_play(Slot& slot);

    Hooks m_hooks;
    int   m_volume;
    Gtk::Box   m_root{Gtk::Orientation::VERTICAL, 10};
    Gtk::Box   m_units{Gtk::Orientation::HORIZONTAL, 12};
    Gtk::Label m_notice;
    Gtk::Label m_help;
    std::vector<std::unique_ptr<Slot>> m_slots;
    std::array<bool, 4> m_seen_screen{};   // the running game drew on that VMU
};

} // namespace ds::frontend
