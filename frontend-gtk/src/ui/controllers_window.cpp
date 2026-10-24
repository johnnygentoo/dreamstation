// SPDX-License-Identifier: GPL-3.0-or-later
#include "controllers_window.hpp"
#include "../hotkeys.hpp"
#include "../host/pad_input.hpp"

#include <gdk/gdkkeysyms.h>
#include <dreamstation/engine/engine.hpp>

#include <map>

namespace ds::frontend {

namespace {

const char* const kTypeIds[]    = {"controller", "arcade", "twinstick", "racing", "none"};
const char* const kTypeLabels[] = {"Controller", "Arcade Stick", "Twin Stick", "Racing Controller",
                                   "None (unplugged)"};
const char* kIdleHint =
    "Click a binding, then press a key or a controller button. "
    "Escape cancels; Backspace clears the binding.";

std::string port_letter(unsigned port) { return std::string(1, static_cast<char>('A' + port)); }

} // namespace

ControllersWindow::ControllersWindow(Config& config) : m_config(config) {
    set_title("DreamStation Controller Settings");
    set_default_size(900, 720);
    add_css_class("ds-settings");

    m_sidebar.set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_sidebar.add_css_class("navigation-sidebar");
    m_sidebar.signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
        if (!row) return;
        end_capture(true);
        const auto index = static_cast<std::size_t>(row->get_index());
        if (index < m_page_ids.size())
            m_stack.set_visible_child(m_page_ids[index]);
    });
    auto* side_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    side_scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    side_scroll->set_child(m_sidebar);
    side_scroll->set_size_request(220, -1);
    m_stack.set_hexpand(true);
    m_stack.set_vexpand(true);
    m_body.append(*side_scroll);
    m_body.append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    m_body.append(m_stack);
    m_body.set_vexpand(true);

    auto* help = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    help->add_css_class("ds-settings-help");
    help->set_margin(12);
    m_hint.set_xalign(0.f);
    m_hint.set_wrap(true);
    m_hint.set_text(kIdleHint);
    help->append(m_hint);

    auto* footer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    footer->set_margin(12);
    auto* spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    auto* close_btn = Gtk::make_managed<Gtk::Button>("Close");
    close_btn->signal_clicked().connect([this] { close(); });
    footer->append(*spacer);
    footer->append(*close_btn);

    m_root.append(m_body);
    m_root.append(*help);
    m_root.append(*footer);
    set_child(m_root);

    for (unsigned port = 0; port < 4; ++port)
        build_port_page(port);
    build_hotkeys_page();
    if (auto* first = m_sidebar.get_row_at_index(0))
        m_sidebar.select_row(*first);

    // Keys pressed while capturing become the binding.
    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) { return on_capture_key(keyval); }, false);
    add_controller(keys);

    // Pads can be plugged in while the window is open.
    m_device_timer = Glib::signal_timeout().connect([this] {
        if (!m_capturing) {
            PadInput::instance().poll_capture();   // pumps hot-plug events
            refresh_device_labels();
            for (auto& refresh : m_refreshers) refresh();   // a pad's own button names
        }
        refresh_game_inputs();
        return true;
    }, 1000);
    signal_hide().connect([this] { end_capture(true); });
}

ControllersWindow::~ControllersWindow() {
    m_capture_timer.disconnect();
    m_device_timer.disconnect();
}

void ControllersWindow::show_page(const std::string& id) {
    for (std::size_t i = 0; i < m_page_ids.size(); ++i)
        if (m_page_ids[i] == id)
            if (auto* row = m_sidebar.get_row_at_index(static_cast<int>(i)))
                m_sidebar.select_row(*row);
    refresh_game_inputs();
    present();
}

// While an arcade game runs, each input says what it does in that game
// ("Punch"), since its buttons are not the Dreamcast pad's.
void ControllersWindow::refresh_game_inputs() {
    const auto arcade = ds::engine::Engine::instance().arcade_inputs();
    using namespace ds::engine;
    static const std::map<std::string, PadButton> kButtons = {
        {"a", PAD_B}, {"b", PAD_A}, {"x", PAD_Y}, {"y", PAD_X}, {"start", PAD_START},
        {"c", PAD_L}, {"z", PAD_R}, {"d", PAD_SELECT}, {"ltrigger", PAD_L2},
        {"rtrigger", PAD_R2}, {"test", PAD_L3}, {"service", PAD_R3},
    };
    for (const auto& [row, id] : m_input_rows) {
        std::string what;
        if (arcade) {
            const bool left_stick  = id.rfind("stick_", 0) == 0;
            const bool right_stick = id.rfind("rstick_", 0) == 0;
            if (const auto it = kButtons.find(id); it != kButtons.end())
                what = arcade->buttons[it->second];
            else if (id == "up" || id == "down" || id == "left" || id == "right")
                what = "Joystick";
            else if (left_stick)
                // Games without an analog control of their own take the
                // stick as the joystick.
                what = arcade->axes[0].empty() && arcade->axes[1].empty()
                           ? "Joystick"
                           : arcade->axes[id == "stick_up" || id == "stick_down" ? 1 : 0];
            else if (right_stick)
                what = arcade->axes[id == "rstick_up" || id == "rstick_down" ? 3 : 2];
            if (what.empty())
                what = "Not used by this game";
            else if (what == row->get_title())
                what.clear();                     // "Start" is just Start
        }
        if (row->get_subtitle() != what)
            row->set_subtitle(what);
    }
}

Gtk::Box& ControllersWindow::add_page(const std::string& id, const std::string& title,
                                      const char* icon) {
    auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    row_box->set_margin(8);
    auto* image = Gtk::make_managed<Gtk::Image>();
    image->set_from_icon_name(icon);
    auto* label = Gtk::make_managed<Gtk::Label>(title);
    label->set_xalign(0.f);
    row_box->append(*image);
    row_box->append(*label);
    m_sidebar.append(*row_box);
    m_page_ids.push_back(id);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 18);
    box->set_margin(18);
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_child(*box);
    m_stack.add(*scroll, id);
    return *box;
}

Gtk::ListBox& ControllersWindow::add_group(Gtk::Box& page, const std::string& title) {
    auto* wrap = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    auto* heading = Gtk::make_managed<Gtk::Label>(title);
    heading->set_xalign(0.f);
    heading->add_css_class("heading");
    wrap->append(*heading);
    auto* list = Gtk::make_managed<Gtk::ListBox>();
    list->set_selection_mode(Gtk::SelectionMode::NONE);
    list->add_css_class("boxed-list");
    wrap->append(*list);
    page.append(*wrap);
    return *list;
}

// ── Port pages ────────────────────────────────────────────────────────────

void ControllersWindow::build_port_page(unsigned port) {
    Gtk::Box& page = add_page("port" + std::to_string(port),
                              "Controller Port " + port_letter(port), "input-gaming-symbolic");
    const std::string type_key = port_letter(port) + ".type";

    auto& device = add_group(page, "Device");
    {
        auto* row = Gtk::make_managed<Adw::ActionRow>();
        row->set_title("Controller Mapped To");
        auto* lbl = Gtk::make_managed<Gtk::Label>();
        lbl->add_css_class("dim-label");
        row->add_suffix(*lbl);
        device.append(*row);
        m_device_labels.push_back(lbl);
    }
    {
        std::vector<Glib::ustring> items(std::begin(kTypeLabels), std::end(kTypeLabels));
        auto* drop = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(items));
        guint selected = 0;
        if (auto it = m_config.bindings.find(type_key); it != m_config.bindings.end())
            for (guint i = 0; i < std::size(kTypeIds); ++i)
                if (it->second == kTypeIds[i]) selected = i;
        drop->set_selected(selected);
        drop->set_valign(Gtk::Align::CENTER);
        drop->property_selected().signal_changed().connect([this, drop, type_key] {
            const guint sel = std::min<guint>(drop->get_selected(), std::size(kTypeIds) - 1);
            if (sel == 0) m_config.bindings.erase(type_key);
            else          m_config.bindings[type_key] = kTypeIds[sel];
            m_config.save();
            signal_changed.emit();
        });
        auto* row = Gtk::make_managed<Adw::ActionRow>();
        row->set_title("Controller Type");
        row->add_suffix(*drop);
        device.append(*row);
    }

    Gtk::ListBox* group = nullptr;
    std::string   group_name;
    for (const PadInputDef& def : pad_input_defs()) {
        if (!group || group_name != def.group) {
            group_name = def.group;
            group = &add_group(page, def.group);
        }
        const std::string key = port_letter(port) + "." + def.id;
        const std::string id  = def.id;
        auto* button = Gtk::make_managed<Gtk::Button>();
        button->set_valign(Gtk::Align::CENTER);
        button->set_size_request(260, -1);
        const auto current = [port, id] {
            return PadInput::instance().describe(PadInput::instance().binding(port, id), port);
        };
        button->set_label(current());
        m_refreshers.push_back([button, current] {
            if (const std::string text = current(); button->get_label() != text)
                button->set_label(text);
        });
        button->signal_clicked().connect([this, button, key, port, current] {
            Capture c;
            c.button  = button;
            c.current = current;
            c.apply   = [this, key](const std::string& source) {
                m_config.bindings[key] = source;       // "" = explicitly unbound
                m_config.save();
                PadInput::instance().set_bindings(m_config.bindings);
                signal_changed.emit();
            };
            begin_capture(std::move(c), port);
        });
        auto* row = Gtk::make_managed<Adw::ActionRow>();
        row->set_title(def.label);
        row->add_suffix(*button);
        group->append(*row);
        m_input_rows.push_back({row, def.id});
    }

    // Automatic mapping = back to the positional defaults; clear = unbind all.
    auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* automatic = Gtk::make_managed<Gtk::Button>("Automatic Mapping");
    automatic->signal_clicked().connect([this, port] {
        for (const auto& def : pad_input_defs())
            m_config.bindings.erase(port_letter(port) + "." + def.id);
        m_config.save();
        PadInput::instance().set_bindings(m_config.bindings);
        for (auto& refresh : m_refreshers) refresh();
        signal_changed.emit();
    });
    auto* clear = Gtk::make_managed<Gtk::Button>("Clear Mapping");
    clear->signal_clicked().connect([this, port] {
        for (const auto& def : pad_input_defs())
            m_config.bindings[port_letter(port) + "." + def.id] = "";
        m_config.save();
        PadInput::instance().set_bindings(m_config.bindings);
        for (auto& refresh : m_refreshers) refresh();
        signal_changed.emit();
    });
    buttons->append(*automatic);
    buttons->append(*clear);
    page.append(*buttons);

    refresh_device_labels();
}

void ControllersWindow::refresh_device_labels() {
    for (std::size_t port = 0; port < m_device_labels.size(); ++port) {
        const std::string name = PadInput::instance().device_name(static_cast<unsigned>(port));
        std::string text = name.empty() ? "No gamepad" : name;
        if (port == 0)
            text += " + Keyboard";
        m_device_labels[port]->set_text(text);
    }
}

// ── Hotkeys ───────────────────────────────────────────────────────────────

void ControllersWindow::build_hotkeys_page() {
    Gtk::Box& page = add_page("hotkeys", "Hotkeys", "preferences-desktop-keyboard-shortcuts-symbolic");
    Gtk::ListBox* group = nullptr;
    std::string   group_name;
    for (const HotkeyDef& def : hotkey_defs()) {
        if (!group || group_name != def.category) {
            group_name = def.category;
            group = &add_group(page, def.category);
        }
        const std::string id          = def.id;
        const std::string default_key = def.default_key;
        const std::string default_pad = def.default_pad;

        // Keyboard binding
        auto* key_button = Gtk::make_managed<Gtk::Button>();
        key_button->set_valign(Gtk::Align::CENTER);
        key_button->set_size_request(170, -1);
        key_button->set_tooltip_text("Keyboard key");
        const auto current_key = [this, id, default_key] {
            std::string key = default_key;
            if (auto it = m_config.hotkeys.find(id); it != m_config.hotkeys.end())
                key = it->second;
            return key.empty() ? std::string("Key —") : "Key " + key;
        };
        key_button->set_label(current_key());
        key_button->signal_clicked().connect([this, key_button, id, default_key, current_key] {
            Capture c;
            c.button  = key_button;
            c.mode    = Capture::Mode::Keys;
            c.current = current_key;
            c.apply   = [this, id, default_key](const std::string& source) {
                const std::string key = source.rfind("key:", 0) == 0 ? source.substr(4) : source;
                if (key == default_key) m_config.hotkeys.erase(id);
                else                    m_config.hotkeys[id] = key;
                m_config.save();
                signal_changed.emit();
            };
            begin_capture(std::move(c), 0);
        });

        // Gamepad binding (port A's pad)
        auto* pad_button = Gtk::make_managed<Gtk::Button>();
        pad_button->set_valign(Gtk::Align::CENTER);
        pad_button->set_size_request(170, -1);
        pad_button->set_tooltip_text("Controller button (port A)");
        const auto current_pad = [this, id, default_pad] {
            std::string pad = default_pad;
            if (auto it = m_config.hotkey_pads.find(id); it != m_config.hotkey_pads.end())
                pad = it->second;
            return pad.empty() ? std::string("Pad —") : "Pad " + pad;
        };
        pad_button->set_label(current_pad());
        pad_button->signal_clicked().connect([this, pad_button, id, default_pad, current_pad] {
            Capture c;
            c.button  = pad_button;
            c.mode    = Capture::Mode::PadButtons;
            c.current = current_pad;
            c.apply   = [this, id, default_pad](const std::string& source) {
                const std::string pad = source.rfind("pad:", 0) == 0 ? source.substr(4) : source;
                if (pad == default_pad) m_config.hotkey_pads.erase(id);
                else                    m_config.hotkey_pads[id] = pad;
                m_config.save();
                signal_changed.emit();
            };
            begin_capture(std::move(c), 0);
        });

        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        box->append(*key_button);
        box->append(*pad_button);
        auto* row = Gtk::make_managed<Adw::ActionRow>();
        row->set_title(def.label);
        row->add_suffix(*box);
        group->append(*row);
    }
}

// ── Capture ───────────────────────────────────────────────────────────────

void ControllersWindow::begin_capture(Capture capture, unsigned port) {
    end_capture(true);
    m_capture   = std::move(capture);
    m_capturing = true;
    m_capture.button->set_label(m_capture.mode == Capture::Mode::Keys ? "Press a key…"
                                : m_capture.mode == Capture::Mode::PadButtons ? "Press a button…"
                                : "Press a key or button…");
    m_hint.set_text("Waiting for input — Escape cancels, Backspace clears.");
    if (m_capture.mode != Capture::Mode::Keys) {
        PadInput::instance().begin_capture(port);
        m_capture_timer = Glib::signal_timeout().connect([this] {
            if (!m_capturing)
                return false;
            if (auto source = PadInput::instance().poll_capture()) {
                // A hotkey takes a button, not an axis.
                if (m_capture.mode == Capture::Mode::PadButtons && source->rfind("pad:", 0) != 0)
                    return true;
                m_capture.apply(*source);
                end_capture(true);
                return false;
            }
            return true;
        }, 30);
    }
}

void ControllersWindow::end_capture(bool restore_label) {
    m_capture_timer.disconnect();
    if (m_capturing && restore_label && m_capture.button && m_capture.current)
        m_capture.button->set_label(m_capture.current());
    m_capturing = false;
    m_capture   = {};
    m_hint.set_text(kIdleHint);
}

bool ControllersWindow::on_capture_key(guint keyval) {
    if (!m_capturing)
        return false;
    if (keyval == GDK_KEY_Escape) {
        end_capture(true);
        return true;
    }
    if (keyval == GDK_KEY_BackSpace || keyval == GDK_KEY_Delete) {
        m_capture.apply("");
        end_capture(true);
        return true;
    }
    // A pad-only binding ignores ordinary keys.
    if (m_capture.mode == Capture::Mode::PadButtons)
        return true;
    // Bare modifiers are not bindable on their own.
    switch (keyval) {
        case GDK_KEY_Shift_L: case GDK_KEY_Shift_R: case GDK_KEY_Control_L:
        case GDK_KEY_Control_R: case GDK_KEY_Alt_L: case GDK_KEY_Alt_R:
        case GDK_KEY_Super_L: case GDK_KEY_Super_R:
            return true;
        default: break;
    }
    const char* name = gdk_keyval_name(gdk_keyval_to_lower(keyval));
    if (!name)
        return true;
    m_capture.apply(std::string("key:") + name);
    end_capture(true);
    return true;
}

} // namespace ds::frontend
