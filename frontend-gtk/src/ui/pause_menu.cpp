// SPDX-License-Identifier: GPL-3.0-or-later
#include "pause_menu.hpp"
#include "../host/pad_input.hpp"

#include <ctime>

namespace ds::frontend {

namespace {
constexpr int kSlots       = 10;
constexpr int kSlotColumns = 5;
}

PauseMenu::PauseMenu() : Gtk::Box(Gtk::Orientation::HORIZONTAL, 0) {
    add_css_class("ds-pause-menu");
    set_hexpand(true);
    set_vexpand(true);
    set_visible(false);

    // Left: the menu itself.  Right: what is being played.
    auto* left = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    left->set_margin(48);
    left->set_valign(Gtk::Align::CENTER);
    left->set_halign(Gtk::Align::START);
    m_stack.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    m_stack.set_transition_duration(120);
    m_stack.add(m_main, "main");
    m_stack.add(m_close, "close");
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_child(m_slots);
    scroll->set_propagate_natural_height(true);
    scroll->set_propagate_natural_width(true);
    m_stack.add(*scroll, "slots");
    left->append(m_stack);
    append(*left);

    auto* right = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    right->set_margin(48);
    right->set_valign(Gtk::Align::START);
    right->set_halign(Gtk::Align::END);
    right->set_hexpand(true);
    m_title.add_css_class("ds-pause-title");
    m_title.set_xalign(1.f);
    m_title.set_wrap(true);
    m_title.set_max_width_chars(32);
    m_subtitle.set_xalign(1.f);
    m_times.set_xalign(1.f);
    m_times.set_justify(Gtk::Justification::RIGHT);
    m_clock.set_xalign(1.f);
    m_clock.add_css_class("ds-pause-title");
    right->append(m_title);
    right->append(m_subtitle);
    right->append(m_times);
    right->append(m_clock);
    append(*right);

    m_main.set_size_request(360, -1);
    m_close.set_size_request(360, -1);
}

PauseMenu::~PauseMenu() { m_pad_timer.disconnect(); }

Gtk::Button* PauseMenu::add_button(Gtk::Box& box, const std::string& label,
                                   const std::string& action) {
    auto* button = Gtk::make_managed<Gtk::Button>(label);
    button->add_css_class("ds-pause-button");
    if (auto* child = dynamic_cast<Gtk::Label*>(button->get_child()))
        child->set_xalign(0.f);
    button->signal_clicked().connect([this, action] {
        if (action == "view:load")       { build_slots(false); show_view("slots"); }
        else if (action == "view:save")  { build_slots(true);  show_view("slots"); }
        else if (action == "view:close") { build_close();      show_view("close"); }
        else if (action == "view:discs") { build_discs();      show_view("close"); }
        else if (action == "view:main")  { build_main();       show_view("main"); }
        else                             signal_action.emit(action);
    });
    box.append(*button);
    m_focus.push_back(button);
    return button;
}

void PauseMenu::build_main() {
    while (auto* c = m_main.get_first_child()) m_main.remove(*c);
    m_focus.clear();
    m_columns = 1;
    add_button(m_main, "Resume Game", "resume");
    m_ff_button = add_button(m_main, m_info.fast_forward ? "Fast Forward: On" : "Fast Forward: Off",
                             "fast-forward");
    auto* load = add_button(m_main, "Load State", "view:load");
    auto* save = add_button(m_main, "Save State", "view:save");
    load->set_sensitive(m_info.can_save_states);
    save->set_sensitive(m_info.can_save_states);
    add_button(m_main, "Game Properties", "properties");
    add_button(m_main, "Save Screenshot", "screenshot");
    auto* disc = add_button(m_main, "Change Disc", "view:discs");
    disc->set_sensitive(m_info.can_save_states);
    add_button(m_main, "Close Game", "view:close");
}

void PauseMenu::build_close() {
    while (auto* c = m_close.get_first_child()) m_close.remove(*c);
    m_focus.clear();
    m_columns = 1;
    add_button(m_close, "Back To Pause Menu", "view:main");
    add_button(m_close, "Restart Game", "restart");
    auto* save = add_button(m_close, "Close and Save State", "close-save");
    save->set_sensitive(m_info.can_save_states);
    add_button(m_close, "Close Without Saving", "close-nosave");
}

// Shares the plain list view with the Close sub-menu.
void PauseMenu::build_discs() {
    while (auto* c = m_close.get_first_child()) m_close.remove(*c);
    m_focus.clear();
    m_columns = 1;
    add_button(m_close, "Back To Pause Menu", "view:main");
    for (const auto& [label, path] : m_info.discs)
        add_button(m_close, label, "disc:" + path);
    add_button(m_close, "From File…", "disc:file");
    add_button(m_close, "Remove Disc", "disc:remove");
}

void PauseMenu::build_slots(bool save) {
    while (auto* c = m_slots.get_first_child()) m_slots.remove(*c);
    m_focus.clear();
    m_columns = kSlotColumns;

    const auto section = [&](bool global) {
        auto* heading = Gtk::make_managed<Gtk::Label>(
            std::string(save ? "Save To " : "Load From ") + (global ? "Global Slot" : "Game Slot"));
        heading->set_xalign(0.f);
        heading->add_css_class("ds-pause-title");
        m_slots.append(*heading);
        auto* grid = Gtk::make_managed<Gtk::Grid>();
        grid->set_row_spacing(10);
        grid->set_column_spacing(10);
        for (int slot = 1; slot <= kSlots; ++slot) {
            const Slot info = m_info.slot ? m_info.slot(global, slot) : Slot{};
            auto* button = Gtk::make_managed<Gtk::Button>();
            button->add_css_class("ds-pause-slot");
            auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
            auto* picture = Gtk::make_managed<Gtk::Picture>();
            picture->set_size_request(176, 132);
            picture->set_content_fit(Gtk::ContentFit::CONTAIN);
            if (info.exists && std::filesystem::exists(info.screenshot))
                picture->set_filename(info.screenshot.string());
            box->append(*picture);
            box->append(*Gtk::make_managed<Gtk::Label>(
                std::string(global ? "Global " : "Game ") + "Slot " + std::to_string(slot)));
            auto* when = Gtk::make_managed<Gtk::Label>(info.exists ? info.when : "Empty");
            when->add_css_class("dim-label");
            box->append(*when);
            button->set_child(*box);
            button->set_sensitive(save || info.exists);
            const std::string action = std::string(save ? "save:" : "load:") +
                                       (global ? "global:" : "game:") + std::to_string(slot);
            button->signal_clicked().connect([this, action] { signal_action.emit(action); });
            grid->attach(*button, (slot - 1) % kSlotColumns, (slot - 1) / kSlotColumns);
            m_focus.push_back(button);
        }
        m_slots.append(*grid);
    };
    section(false);
    section(true);
}

void PauseMenu::show_view(const std::string& name) {
    m_view = name;
    m_stack.set_visible_child(name);
    // Land on the first usable entry.
    m_index = 0;
    for (std::size_t i = 0; i < m_focus.size(); ++i)
        if (m_focus[i]->get_sensitive()) { m_index = i; break; }
    if (!m_focus.empty())
        m_focus[m_index]->grab_focus();
}

void PauseMenu::open(Info info) {
    m_info = std::move(info);
    m_title.set_text(m_info.title);
    m_subtitle.set_text(m_info.subtitle);
    std::string times;
    if (!m_info.session_time.empty()) times += "This session: " + m_info.session_time;
    if (!m_info.total_time.empty())   times += (times.empty() ? "" : "\n") + std::string("All time: ") + m_info.total_time;
    m_times.set_text(times);
    const std::time_t now = std::time(nullptr);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M", std::localtime(&now));
    m_clock.set_text(buf);

    build_main();
    set_visible(true);
    show_view("main");

    m_pad_prev = ~0u;
    m_pad_timer.disconnect();
    m_pad_timer = Glib::signal_timeout().connect(sigc::mem_fun(*this, &PauseMenu::poll_pad), 33);
}

void PauseMenu::close_menu() {
    m_pad_timer.disconnect();
    set_visible(false);
}

bool PauseMenu::go_back() {
    if (m_view == "main")
        return false;
    build_main();
    show_view("main");
    return true;
}

void PauseMenu::move_focus(int dx, int dy) {
    if (m_focus.empty())
        return;
    const int count = static_cast<int>(m_focus.size());
    int index = static_cast<int>(m_index);
    const int step = m_columns > 1 ? dx + dy * m_columns : (dy != 0 ? dy : dx);
    // Skip over disabled entries, wrapping around once.
    for (int tries = 0; tries < count; ++tries) {
        index = ((index + step) % count + count) % count;
        if (m_focus[static_cast<std::size_t>(index)]->get_sensitive())
            break;
    }
    m_index = static_cast<std::size_t>(index);
    m_focus[m_index]->grab_focus();
}

// Controller navigation: act on newly pressed buttons only.
bool PauseMenu::poll_pad() {
    if (!get_visible())
        return false;
    const unsigned now     = PadInput::instance().nav_state();
    const unsigned pressed = now & ~m_pad_prev;
    const bool first       = m_pad_prev == ~0u;
    m_pad_prev = now;
    if (first)
        return true;

    // Keep our index in step with focus moved by keyboard or mouse.
    for (std::size_t i = 0; i < m_focus.size(); ++i)
        if (m_focus[i]->has_focus()) { m_index = i; break; }

    if (pressed & PadInput::NavUp)    move_focus(0, -1);
    if (pressed & PadInput::NavDown)  move_focus(0, +1);
    if (pressed & PadInput::NavLeft)  move_focus(-1, 0);
    if (pressed & PadInput::NavRight) move_focus(+1, 0);
    if ((pressed & PadInput::NavAccept) && m_index < m_focus.size())
        m_focus[m_index]->activate();
    if (pressed & PadInput::NavBack) {
        if (!go_back())
            signal_action.emit("resume");
    }
    return true;
}

} // namespace ds::frontend
