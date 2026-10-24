// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gtkmm.h>
#include <filesystem>
#include <vector>

#include <dreamstation/engine/engine.hpp>

#include "../config.hpp"
#include "../game_library.hpp"

namespace ds::frontend {

// System > Online Play: two players on two computers play one game together
// (GGPO rollback netplay). One hosts, the other joins the host's address;
// both pick the same game.
class NetplayDialog : public Gtk::Window {
public:
    NetplayDialog(Config& config, const GameLibrary& library,
                  const std::filesystem::path& preselect);

    // Start was pressed: the game and how to connect.
    sigc::signal<void(std::filesystem::path, ds::engine::Engine::Netplay)> signal_start;

private:
    void update_mode();
    void start();

    Config& m_config;
    std::vector<std::filesystem::path> m_paths;   // per drop-down entry

    Gtk::Box         m_root{Gtk::Orientation::VERTICAL, 12};
    Gtk::DropDown    m_game;
    Gtk::ToggleButton m_host{"Host"}, m_join{"Join"};
    Gtk::Entry       m_address;
    Gtk::SpinButton  m_delay;
    Gtk::Label       m_info;
    Gtk::Button      m_start{"Start"};
};

} // namespace ds::frontend
