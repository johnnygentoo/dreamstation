// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gtkmm.h>
#include <filesystem>
#include <string>

#include "vmu_unit.hpp"

namespace ds::frontend {

// A window playing one VMU mini-game from a card image, .vms or .dci file
// (Tools > Play VMU Mini-Game). Progress is saved back to that file when the
// game exits or the window closes.
class VmuPlayer : public Gtk::Window {
public:
    // `volume` 0..200 percent. Check ok() after construction.
    VmuPlayer(const std::filesystem::path& source, int volume);

    bool ok() const { return m_ok; }
    const std::string& error() const { return m_error; }

    // For scripted test runs: hold `buttons` (VmuCpu::Button bits).
    void test_hold(unsigned buttons) { m_unit.test_hold(buttons); }

private:
    Gtk::Box    m_root{Gtk::Orientation::VERTICAL, 8};
    VmuUnit     m_unit;
    Gtk::Label  m_help;
    bool        m_ok = false;
    std::string m_error;
};

} // namespace ds::frontend
