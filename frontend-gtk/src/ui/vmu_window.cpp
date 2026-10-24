// SPDX-License-Identifier: GPL-3.0-or-later
#include "vmu_window.hpp"
#include "../vmu.hpp"

#include <algorithm>

namespace ds::frontend {

namespace fs = std::filesystem;

namespace {

std::string port_name(int port) {
    return std::string(1, static_cast<char>('A' + port)) + "1";
}

// The mini-game on a card, if any.
std::optional<VmuImage::File> card_game(const fs::path& card) {
    const auto image = VmuImage::load(card);
    if (!image || !image->valid())
        return std::nullopt;
    for (const auto& f : image->files())
        if (f.game)
            return f;
    return std::nullopt;
}

} // namespace

VmuWindow::VmuWindow(Hooks hooks, int volume) : m_hooks(std::move(hooks)), m_volume(volume) {
    set_title("VMU");
    set_default_size(320, 560);
    add_css_class("ds-settings");

    m_root.set_margin(12);
    m_notice.set_wrap(true);
    m_notice.set_justify(Gtk::Justification::CENTER);
    m_notice.set_visible(false);
    m_root.append(m_notice);
    m_units.set_homogeneous(true);
    m_units.set_vexpand(true);
    m_root.append(m_units);
    m_help.set_wrap(true);
    m_help.set_justify(Gtk::Justification::CENTER);
    m_help.add_css_class("dim-label");
    m_help.set_text("While a mini-game plays, its VMU is out of the controller and the game is "
                    "paused.\nArrows / WASD  ·  Z or Enter: A  ·  X: B  ·  M: Mode  ·  Q: Sleep  ·  "
                    "or the gamepad while this window is in front");
    m_root.append(m_help);
    set_child(m_root);

    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect([this](guint keyval, guint, Gdk::ModifierType) {
        bool used = false;
        for (auto& s : m_slots)
            used |= s->unit->key(keyval, true);
        return used;
    }, false);
    keys->signal_key_released().connect([this](guint keyval, guint, Gdk::ModifierType) {
        for (auto& s : m_slots)
            s->unit->key(keyval, false);
    });
    add_controller(keys);

    refresh();
}

VmuWindow::~VmuWindow() {
    stop_all();
}

VmuWindow::Slot* VmuWindow::slot_for(int port) {
    for (auto& s : m_slots)
        if (s->port == port)
            return s.get();
    return nullptr;
}

void VmuWindow::refresh() {
    const bool running = m_hooks.game_running();
    if (!running)
        m_seen_screen.fill(false);
    for (int port = 0; port < 4; ++port) {
        // Controller A's VMU always shows; the others once the game has drawn
        // on them or when they hold a mini-game, so unused ones take no room.
        const fs::path card = m_hooks.card_path(port);
        const bool wanted = port == 0 || m_seen_screen[static_cast<std::size_t>(port)] ||
                            card_game(card).has_value();
        Slot* slot = slot_for(port);
        if (!wanted) {
            if (slot && !slot->unit->playing() && !slot->out) {
                m_units.remove(slot->box);
                std::erase_if(m_slots, [port](const auto& s) { return s->port == port; });
            }
            continue;
        }
        if (!slot) {
            auto s = std::make_unique<Slot>();
            s->port = port;
            s->unit = std::make_unique<VmuUnit>(m_volume);
            s->title.add_css_class("heading");
            s->box.append(s->title);
            s->box.append(*s->unit);
            s->play.set_halign(Gtk::Align::CENTER);
            s->box.append(s->play);
            Slot* raw = s.get();
            s->play.signal_clicked().connect([this, raw] { toggle_play(*raw); });
            s->unit->signal_ended.connect([raw] {
                if (raw->out)
                    raw->unit->set_status("Progress saved. Plug the VMU back in to carry on with the game.");
            });
            // Keep the controller order whatever order they appear in.
            Gtk::Widget* after = nullptr;
            for (auto& other : m_slots)
                if (other->port < port)
                    after = &other->box;
            if (after)
                m_units.insert_child_after(s->box, *after);
            else
                m_units.prepend(s->box);
            m_slots.push_back(std::move(s));
            slot = raw;
        }
        update_slot(*slot);
    }
}

void VmuWindow::update_slot(Slot& slot) {
    if (!m_hooks.game_running())
        slot.out = false;                 // no game to plug it back into
    const fs::path card = m_hooks.card_path(slot.port);
    const auto game = slot.unit->playing() ? std::nullopt : card_game(card);
    if (!slot.unit->playing())
        slot.has_game = game.has_value();
    slot.title.set_text("VMU " + port_name(slot.port) +
                        (slot.unit->playing() ? " — " + slot.unit->game_title()
                         : game ? " — " + (game->long_description.empty() ? game->name : game->long_description)
                                : ""));
    if (slot.unit->playing()) {
        slot.play.set_label(slot.out ? "Plug Back In" : "Stop");
        slot.play.set_sensitive(true);
        slot.play.add_css_class("suggested-action");
    } else {
        slot.play.set_label("Play Mini-Game");
        slot.play.set_sensitive(slot.has_game);
        slot.play.remove_css_class("suggested-action");
        slot.play.set_tooltip_text(slot.has_game ? "" : "There is no mini-game on this VMU");
    }
}

void VmuWindow::show_screen(int port, const Dots* dots) {
    if (port < 0 || port > 3)
        return;
    // Games clear every VMU's screen; only one showing something counts.
    if (dots && !m_seen_screen[static_cast<std::size_t>(port)] &&
        std::any_of(dots->begin(), dots->end(), [](std::uint8_t d) { return d != 0; })) {
        m_seen_screen[static_cast<std::size_t>(port)] = true;
        refresh();
    }
    if (Slot* slot = slot_for(port))
        slot->unit->show_screen(dots ? dots->data() : nullptr);
}

void VmuWindow::announce(int port, const std::string& game_title) {
    refresh();
    m_notice.set_text(game_title + " is now on VMU " + port_name(port) +
                      ". Press Play Mini-Game to take it out and play.");
    m_notice.set_visible(true);
}

void VmuWindow::toggle_play(Slot& slot) {
    if (slot.unit->playing()) {
        end_play(slot);
        return;
    }
    std::string error;
    if (!start_play(slot, error))
        slot.unit->set_status(error);
}

bool VmuWindow::start_play(Slot& slot, std::string& error) {
    // One mini-game at a time: the keys and the pad drive it.
    for (auto& other : m_slots)
        if (other.get() != &slot && other->unit->playing())
            end_play(*other);
    const bool running = m_hooks.game_running();
    if (running) {
        if (!m_hooks.take_out(slot.port, true)) {
            error = "The VMU cannot be taken out now.";
            return false;
        }
        slot.out = true;
    }
    if (!slot.unit->play(m_hooks.card_path(slot.port), error)) {
        if (slot.out) {
            m_hooks.take_out(slot.port, false);
            slot.out = false;
        }
        return false;
    }
    m_notice.set_visible(false);
    slot.unit->set_status(running ? "Taken out of the controller; the game is paused." : "");
    update_slot(slot);
    present();
    return true;
}

void VmuWindow::end_play(Slot& slot) {
    slot.unit->stop();                    // saves its progress to the card
    if (slot.out) {
        slot.out = false;
        m_hooks.take_out(slot.port, false);
        slot.unit->set_status("Plugged back in.");
    } else {
        slot.unit->set_status("");
    }
    update_slot(slot);
}

void VmuWindow::stop_all() {
    for (auto& s : m_slots)
        if (s->unit->playing() || s->out)
            end_play(*s);
}

bool VmuWindow::test_toggle(int port, std::string& error) {
    Slot* slot = slot_for(port);
    if (!slot) {
        error = "no VMU " + port_name(port);
        return false;
    }
    if (slot->unit->playing()) {
        end_play(*slot);
        return true;
    }
    return start_play(*slot, error);
}

void VmuWindow::test_hold(unsigned buttons) {
    for (auto& s : m_slots)
        if (s->unit->playing())
            s->unit->test_hold(buttons);
}

} // namespace ds::frontend
