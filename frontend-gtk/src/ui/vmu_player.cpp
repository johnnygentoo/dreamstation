// SPDX-License-Identifier: GPL-3.0-or-later
#include "vmu_player.hpp"

namespace ds::frontend {

VmuPlayer::VmuPlayer(const std::filesystem::path& source, int volume) : m_unit(volume) {
    set_title("VMU");
    set_default_size(460, 700);
    add_css_class("ds-settings");
    if (!m_unit.play(source, m_error))
        return;
    m_ok = true;
    set_title("VMU — " + m_unit.game_title());

    m_root.set_margin(12);
    m_root.append(m_unit);
    m_help.set_wrap(true);
    m_help.set_justify(Gtk::Justification::CENTER);
    m_help.add_css_class("dim-label");
    m_help.set_text("Arrows / WASD move  ·  Z or Enter: A  ·  X: B  ·  M: Mode  ·  Q: Sleep\n"
                    "Gamepad: D-pad, south = A, east = B, Start = Mode, Back = Sleep  ·  or click the buttons");
    m_root.append(m_help);
    set_child(m_root);

    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect([this](guint keyval, guint, Gdk::ModifierType) {
        return m_unit.key(keyval, true);
    }, false);
    keys->signal_key_released().connect([this](guint keyval, guint, Gdk::ModifierType) {
        m_unit.key(keyval, false);
    });
    add_controller(keys);

    // The unit saves progress when it stops.
    signal_close_request().connect([this] {
        m_unit.stop();
        return false;
    }, false);
}

} // namespace ds::frontend
