// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_window.hpp"

#include "post_chain.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <set>
#include <cmath>
#include <cstdio>

namespace ds::frontend {

using ds::engine::OptionInfo;

namespace {

const char* kDefaultHelpTitle = "Settings";
const char* kDefaultHelpText  =
    "Hover over an option to see what it does. Changes apply immediately.";

bool is_toggle(const OptionInfo& opt) {
    if (opt.values.size() != 2)
        return false;
    const auto has = [&](const char* v) {
        return std::find(opt.values.begin(), opt.values.end(), v) != opt.values.end();
    };
    return has("enabled") && has("disabled");
}

std::string label_for(const OptionInfo& opt, const std::string& value) {
    for (std::size_t i = 0; i < opt.values.size(); ++i)
        if (opt.values[i] == value)
            return opt.value_labels[i];
    return value;
}

} // namespace

// ── Construction ──────────────────────────────────────────────────────────

SettingsWindow::SettingsWindow(Config& config, std::vector<OptionInfo> options,
                               std::optional<GameContext> game)
    : m_config(config), m_options(std::move(options)), m_game(std::move(game)) {
    if (m_game) {
        m_game_id       = m_game->id;
        m_game_settings = GameSettings::load(m_game_id);
        set_title(m_game->title + " [" + m_game_id + ".toml]");
    } else {
        set_title("DreamStation Settings");
    }
    set_default_size(980, 720);
    add_css_class("ds-settings");

    // Sidebar
    m_sidebar.set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_sidebar.add_css_class("navigation-sidebar");
    m_sidebar.signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
        if (!row)
            return;
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
    m_stack.set_transition_type(Gtk::StackTransitionType::CROSSFADE);

    m_body.append(*side_scroll);
    m_body.append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    m_body.append(m_stack);
    m_body.set_vexpand(true);

    // Help panel
    auto* help = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    help->add_css_class("ds-settings-help");
    help->set_margin(12);
    help->set_size_request(-1, 96);
    help->set_vexpand(false);
    m_help_title.set_xalign(0.f);
    m_help_title.add_css_class("heading");
    m_help_text.set_xalign(0.f);
    m_help_text.set_yalign(0.f);
    m_help_text.set_wrap(true);
    help->append(m_help_title);
    help->append(m_help_text);
    set_help({}, {});

    // Footer
    auto* footer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    footer->set_margin(12);
    auto* reset = Gtk::make_managed<Gtk::Button>(m_game ? "Clear Settings" : "Restore Defaults");
    reset->set_tooltip_text(m_game ? "Remove every override for this game"
                                   : "Reset every engine option to its default");
    reset->signal_clicked().connect([this] {
        if (m_game) {
            m_game_settings.engine.clear();
            m_game_settings.save(m_game_id);
        } else {
            m_config.engine.clear();
            m_config.save();
        }
        signal_reset.emit();
    });
    auto* spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    auto* close_btn = Gtk::make_managed<Gtk::Button>("Close");
    close_btn->signal_clicked().connect([this] { close(); });
    footer->append(*reset);
    footer->append(*spacer);
    footer->append(*close_btn);

    m_root.append(m_body);
    m_root.append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    m_root.append(*help);
    m_root.append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    m_root.append(*footer);
    set_child(m_root);

    // Pages, in DuckStation's sidebar order.
    if (m_game) {
        build_summary_page();
    } else {
        build_interface_page();
        build_game_list_page();
        build_bios_page();
    }
    build_console_page();
    build_online_page();
    build_emulation_page();
    build_vmu_page();
    build_graphics_page();
    if (!m_game) {
        build_osd_page();
        build_post_processing_page();
        build_audio_page();
    }
    build_controllers_page();
    build_advanced_page();

    if (auto* first = m_sidebar.get_row_at_index(0))
        m_sidebar.select_row(*first);
}

void SettingsWindow::show_page(const Glib::ustring& id) {
    // Old page names from the menu map onto the new layout.
    std::string want = id;
    if (m_game && (want == "interface" || want == "bios" || want == "gamelist"))
        want = "summary";
    for (std::size_t i = 0; i < m_page_ids.size(); ++i) {
        if (m_page_ids[i] == want) {
            if (auto* row = m_sidebar.get_row_at_index(static_cast<int>(i)))
                m_sidebar.select_row(*row);
            break;
        }
    }
    present();
}

void SettingsWindow::set_help(const std::string& title, const std::string& text) {
    m_help_title.set_text(title.empty() ? kDefaultHelpTitle : title);
    m_help_text.set_text(text.empty() ? kDefaultHelpText : text);
}

// ── Building blocks ───────────────────────────────────────────────────────

SettingsWindow::Page SettingsWindow::add_page(const std::string& id, const std::string& title,
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

    return Page{box};
}

Gtk::ListBox& SettingsWindow::add_group(Page& page, const std::string& title,
                                        const std::string& description) {
    auto* wrap = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    auto* heading = Gtk::make_managed<Gtk::Label>(title);
    heading->set_xalign(0.f);
    heading->add_css_class("heading");
    wrap->append(*heading);
    if (!description.empty()) {
        auto* desc = Gtk::make_managed<Gtk::Label>(description);
        desc->set_xalign(0.f);
        desc->set_wrap(true);
        desc->add_css_class("dim-label");
        wrap->append(*desc);
    }
    auto* list = Gtk::make_managed<Gtk::ListBox>();
    list->set_selection_mode(Gtk::SelectionMode::NONE);
    list->add_css_class("boxed-list");
    wrap->append(*list);
    page.box->append(*wrap);
    return *list;
}

Adw::ActionRow* SettingsWindow::add_row(Gtk::ListBox& group, const std::string& title,
                                        const std::string& help, Gtk::Widget* suffix) {
    auto* row = Gtk::make_managed<Adw::ActionRow>();
    row->set_title(Glib::Markup::escape_text(title));
    if (suffix) {
        suffix->set_valign(Gtk::Align::CENTER);
        row->add_suffix(*suffix);
    }
    // The help panel follows the pointer, as in DuckStation.
    auto motion = Gtk::EventControllerMotion::create();
    motion->signal_enter().connect([this, title, help](double, double) { set_help(title, help); });
    motion->signal_leave().connect([this] { set_help({}, {}); });
    row->add_controller(motion);
    group.append(*row);
    return row;
}

void SettingsWindow::add_switch(Gtk::ListBox& group, const std::string& title,
                                const std::string& help, bool& value) {
    auto* sw = Gtk::make_managed<Gtk::Switch>();
    sw->set_active(value);
    bool* target = &value;
    sw->property_active().signal_changed().connect([this, sw, target] {
        *target = sw->get_active();
        m_config.save();
        signal_changed.emit();
    });
    auto* row = add_row(group, title, help, sw);
    row->set_activatable_widget(sw);
}

void SettingsWindow::add_path_row(Gtk::ListBox& group, const std::string& title,
                                  const std::string& help, std::filesystem::path& target,
                                  const char* pattern) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* path_lbl = Gtk::make_managed<Gtk::Label>(target.empty() ? "Not configured"
                                                                  : target.string());
    path_lbl->add_css_class("dim-label");
    path_lbl->set_ellipsize(Pango::EllipsizeMode::START);
    path_lbl->set_max_width_chars(44);
    auto* btn = Gtk::make_managed<Gtk::Button>("Browse…");
    std::filesystem::path* dest = &target;
    btn->signal_clicked().connect([this, path_lbl, dest, title, pattern] {
        auto dlg = Gtk::FileDialog::create();
        dlg->set_title("Select " + title);
        auto filter = Gtk::FileFilter::create();
        filter->set_name(title);
        filter->add_pattern(pattern);
        auto filters = Gio::ListStore<Gtk::FileFilter>::create();
        filters->append(filter);
        dlg->set_filters(filters);
        dlg->open(*this, [this, dlg, path_lbl, dest](const Glib::RefPtr<Gio::AsyncResult>& res) {
            try {
                *dest = dlg->open_finish(res)->get_path();
                path_lbl->set_text(dest->string());
                m_config.save();
                signal_changed.emit();
            } catch (...) {}
        });
    });
    box->append(*path_lbl);
    box->append(*btn);
    add_row(group, title, help, box);
}

// ── Engine option layer ───────────────────────────────────────────────────

std::optional<std::string> SettingsWindow::layer_value(const std::string& key) const {
    const auto& map = m_game ? m_game_settings.engine : m_config.engine;
    const auto it = map.find(key);
    if (it == map.end())
        return std::nullopt;
    return it->second;
}

void SettingsWindow::set_layer_value(const std::string& key,
                                     const std::optional<std::string>& value) {
    auto& map = m_game ? m_game_settings.engine : m_config.engine;
    if (value) map[key] = *value;
    else       map.erase(key);
    if (m_game) m_game_settings.save(m_game_id);
    else        m_config.save();
    signal_engine_option.emit(key);
}

std::string SettingsWindow::inherited_value(const OptionInfo& opt) const {
    if (m_game) {
        const auto it = m_config.engine.find(opt.key);
        if (it != m_config.engine.end())
            return it->second;
    }
    return opt.default_value;
}

void SettingsWindow::add_engine_option(Gtk::ListBox& group, const OptionInfo& opt) {
    m_placed[opt.key] = true;
    const std::string key     = opt.key;
    const std::string def     = opt.default_value;
    const auto        current = layer_value(key);

    // Global layer, on/off option: a switch.
    if (!m_game && is_toggle(opt)) {
        auto* sw = Gtk::make_managed<Gtk::Switch>();
        sw->set_active(current.value_or(def) == "enabled");
        sw->property_active().signal_changed().connect([this, sw, key, def] {
            const std::string v = sw->get_active() ? "enabled" : "disabled";
            set_layer_value(key, v == def ? std::nullopt : std::optional<std::string>(v));
        });
        auto* row = add_row(group, opt.label, opt.info, sw);
        row->set_activatable_widget(sw);
        return;
    }

    // Otherwise a drop-down. Per-game, the first entry defers to the global layer.
    std::vector<Glib::ustring> items;
    if (m_game)
        items.push_back("Use Global Setting [" + label_for(opt, inherited_value(opt)) + "]");
    for (const auto& l : opt.value_labels)
        items.push_back(l);
    auto* drop = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(items));

    const guint offset = m_game ? 1u : 0u;
    guint selected = 0;
    if (!m_game || current) {
        const std::string value = current.value_or(def);
        const auto it = std::find(opt.values.begin(), opt.values.end(), value);
        if (it != opt.values.end())
            selected = offset + static_cast<guint>(it - opt.values.begin());
    }
    drop->set_selected(selected);

    const std::vector<std::string> values = opt.values;
    const bool per_game = m_game.has_value();
    drop->property_selected().signal_changed().connect([this, drop, key, def, values, offset, per_game] {
        const guint sel = drop->get_selected();
        if (per_game && sel == 0) {
            set_layer_value(key, std::nullopt);
            return;
        }
        if (sel < offset || sel - offset >= values.size())
            return;
        const std::string& v = values[sel - offset];
        // The global layer stores only what differs from the engine default.
        set_layer_value(key, (!per_game && v == def) ? std::nullopt
                                                     : std::optional<std::string>(v));
    });
    add_row(group, opt.label, opt.info, drop);
}

void SettingsWindow::add_engine_options(Page& page, const std::string& group_title,
                                        const std::vector<std::string>& keys) {
    Gtk::ListBox* group = nullptr;
    for (const auto& key : keys) {
        const auto it = std::find_if(m_options.begin(), m_options.end(),
                                     [&](const OptionInfo& o) { return o.key == key; });
        if (it == m_options.end() || m_placed.count(key))
            continue;
        if (!group)
            group = &add_group(page, group_title);
        add_engine_option(*group, *it);
    }
}

// Everything in an engine category that no page has claimed explicitly, so
// options added by future engine versions still show up somewhere.
// Options a later page shows, kept out of the catch-all groups before it.
static const std::set<std::string> kLaterPages = {"emulate_bba", "dcnet", "upnp"};

void SettingsWindow::add_engine_category(Page& page, const std::string& group_title,
                                         const std::string& category) {
    std::vector<std::string> keys;
    for (const auto& o : m_options)
        if (o.category == category && !m_placed.count(o.key) && !kLaterPages.count(o.key))
            keys.push_back(o.key);
    add_engine_options(page, group_title, keys);
}

// ── Pages ─────────────────────────────────────────────────────────────────

void SettingsWindow::build_summary_page() {
    Page page = add_page("summary", "Summary", "dialog-information-symbolic");
    auto& group = add_group(page, "Game",
        "Settings changed in this window apply to this game only and override the "
        "global settings.");
    const auto info = [&](const char* title, const std::string& value) {
        auto* lbl = Gtk::make_managed<Gtk::Label>(value.empty() ? "—" : value);
        lbl->set_selectable(true);
        lbl->set_ellipsize(Pango::EllipsizeMode::START);
        lbl->set_max_width_chars(52);
        add_row(group, title, {}, lbl);
    };
    info("Title",          m_game->title);
    info("Title on Disc",  m_game->disc_title);
    info("Product Number", m_game->product_id);
    info("Region",         m_game->region);
    info("Image Path",     m_game->path.string());
    info("Settings File",  (Config::gamesettings_dir() / (m_game_id + ".toml")).string());
}

void SettingsWindow::build_interface_page() {
    Page page = add_page("interface", "Interface", "preferences-desktop-symbolic");
    auto& b = m_config.behavior;

    auto& behaviour = add_group(page, "Behaviour");
    add_switch(behaviour, "Confirm Game Close",
        "Ask before closing a running game, so a slip of the hand does not lose progress.",
        b.confirm_close);
    add_switch(behaviour, "Save State On Game Close",
        "Save the game's state when it is closed, and offer to resume from it next time.",
        b.save_state_on_close);
    add_switch(behaviour, "Pause On Focus Loss",
        "Pause the game while another application has focus.", b.pause_on_focus_loss);
    add_switch(behaviour, "Pause When Viewing The Game List",
        "Pause the game while the game list is showing, and resume on return.",
        b.pause_in_library);
    add_switch(behaviour, "Inhibit Screensaver",
        "Keep the screen from blanking or locking while a game is running.",
        b.inhibit_screensaver);

    auto& display = add_group(page, "Game Display");
    add_switch(display, "Start Fullscreen",
        "Switch to fullscreen whenever a game starts.", b.start_fullscreen);
    add_switch(display, "Double-Click Toggles Fullscreen",
        "Double-click the game image to enter or leave fullscreen.",
        b.double_click_fullscreen);
    add_switch(display, "Hide Cursor In Fullscreen",
        "Hide the mouse pointer while a game is running fullscreen.",
        b.hide_cursor_fullscreen);

    auto& look = add_group(page, "Appearance");
    add_switch(look, "Show Toolbar", "Show the emulation controls below the header bar.",
               m_config.iface.show_toolbar);
    add_switch(look, "Show Status Bar",
               "Show the game name, frame rates and resolution at the bottom of the window.",
               m_config.iface.show_status_bar);
}

void SettingsWindow::rebuild_game_dirs() {
    if (!m_game_dirs)
        return;
    while (auto* child = m_game_dirs->get_first_child())
        m_game_dirs->remove(*child);
    for (std::size_t i = 0; i < m_config.paths.game_dirs.size(); ++i) {
        auto* remove = Gtk::make_managed<Gtk::Button>();
        remove->set_icon_name("list-remove-symbolic");
        remove->set_tooltip_text("Remove from the search list");
        remove->signal_clicked().connect([this, i] {
            if (i < m_config.paths.game_dirs.size()) {
                m_config.paths.game_dirs.erase(m_config.paths.game_dirs.begin() +
                                               static_cast<std::ptrdiff_t>(i));
                m_config.save();
                rebuild_game_dirs();
                signal_changed.emit();
            }
        });
        add_row(*m_game_dirs, m_config.paths.game_dirs[i].string(),
                "This directory and everything below it is scanned for games.", remove);
    }
    auto* add = Gtk::make_managed<Gtk::Button>();
    add->set_icon_name("list-add-symbolic");
    add->signal_clicked().connect([this] {
        auto dlg = Gtk::FileDialog::create();
        dlg->set_title("Select Game Directory");
        dlg->select_folder(*this, [this, dlg](const Glib::RefPtr<Gio::AsyncResult>& res) {
            try {
                m_config.paths.game_dirs.emplace_back(dlg->select_folder_finish(res)->get_path());
                m_config.save();
                rebuild_game_dirs();
                signal_changed.emit();
            } catch (...) {}
        });
    });
    add_row(*m_game_dirs, "Add Search Directory…",
            "Add a directory to scan for disc images and arcade ROM sets.", add);

    // Games hidden with "Exclude From List".
    if (m_excluded) {
        while (auto* child = m_excluded->get_first_child())
            m_excluded->remove(*child);
        for (std::size_t i = 0; i < m_config.paths.excluded.size(); ++i) {
            auto* restore = Gtk::make_managed<Gtk::Button>();
            restore->set_icon_name("list-remove-symbolic");
            restore->set_tooltip_text("Show this game in the list again");
            restore->signal_clicked().connect([this, i] {
                if (i < m_config.paths.excluded.size()) {
                    m_config.paths.excluded.erase(m_config.paths.excluded.begin() +
                                                  static_cast<std::ptrdiff_t>(i));
                    m_config.save();
                    Glib::signal_idle().connect_once([this] { rebuild_game_dirs(); });
                    signal_changed.emit();
                }
            });
            add_row(*m_excluded, m_config.paths.excluded[i].string(),
                    "Hidden from the game list. Remove the entry to show it again.", restore);
        }
        if (m_config.paths.excluded.empty()) {
            auto* none = Gtk::make_managed<Gtk::Label>("Nothing is excluded");
            none->add_css_class("dim-label");
            add_row(*m_excluded, "Excluded Paths",
                    "Right-click a game and choose Exclude From List to hide it.", none);
        }
    }
}

void SettingsWindow::build_game_list_page() {
    Page page = add_page("gamelist", "Game List", "applications-games-symbolic");
    m_game_dirs = &add_group(page, "Search Directories",
        "Scanned recursively for GDI, CDI, CHD and CUE images, and for Naomi, Naomi 2 and "
        "Atomiswave ROM sets (zipped, or unzipped into a folder named after the set).");
    m_excluded = &add_group(page, "Excluded Paths (will not be listed)");
    auto& options = add_group(page, "Options");
    add_switch(options, "Refresh Automatically",
        "Rescan when games are added to, removed from or renamed in the search directories, "
        "a few seconds after copying ends (after the game is closed if one is running). "
        "New games get covers like the rest.", m_config.library.watch_dirs);
    add_switch(options, "Download Missing Covers",
        "Fetch covers for games that have none after each scan, from libretro's thumbnail "
        "server. The Cover Downloader in the Tools menu shows the progress.",
        m_config.library.auto_covers);
    add_switch(options, "3D Box Covers",
        "Show the game grid as shelves of game boxes in 3D, from the 3D box pictures ES-DE "
        "has scraped. Games without one show their flat cover (also in the View menu).",
        m_config.library.box_covers);
    rebuild_game_dirs();
}

void SettingsWindow::build_bios_page() {
    Page page = add_page("bios", "BIOS", "media-flash-symbolic");
    auto& group = add_group(page, "Dreamcast Firmware",
        "Arcade BIOS archives (naomi.zip, naomi2.zip, awbios.zip, ...) are picked up "
        "automatically from the folder containing the boot ROM, then from RetroArch's "
        "system folder. Each game gets the BIOS its board needs when it starts.");
    add_path_row(group, "Boot ROM",
        "dc_boot.bin — the console's boot ROM. Required unless HLE BIOS is enabled.",
        m_config.paths.bios, "*.bin");
    add_path_row(group, "Flash ROM",
        "dc_flash.bin — the console's settings memory (language, clock, region). "
        "It is copied once into DreamStation's data folder; your original is not modified.",
        m_config.paths.flash, "*.bin");
    add_engine_options(page, "Options", {"hle_bios"});
}

void SettingsWindow::build_console_page() {
    Page page = add_page("console", "Console", "computer-symbolic");
    add_engine_options(page, "Console", {"region", "language", "broadcast", "cable_type"});
    add_engine_options(page, "CPU & Memory", {"sh4clock", "dc_32mb_mod", "enable_dsp"});
    add_engine_options(page, "GD-ROM", {"gdrom_fast_loading"});
    add_engine_options(page, "Arcade (Naomi / Atomiswave)",
                       {"allow_service_buttons", "force_freeplay", "coin_limit"});
    add_engine_category(page, "Other", "system");
}

void SettingsWindow::build_online_page() {
    Page page = add_page("online", "Online", "network-workgroup-symbolic");

    auto& play = add_group(page, "Online Play With Friends",
        "System > Online Play (or right-click a game > Play Online) plays a game with a friend "
        "on another computer, with rollback so it stays responsive. One of you hosts, the other "
        "joins the host's address; you both need the same game files. The host's UDP port " +
        std::to_string(ds::engine::Engine::kNetplayPort) + " must be reachable: on the same "
        "network it is; over the Internet the router has to pass it on (Enable UPnP asks it to). "
        "Online games start from clean saves and default emulation settings on both sides, and "
        "cannot be paused or use save states.");
    (void)play;

    add_engine_options(page, "Dreamcast Internet",
                       {"emulate_bba", "dcnet", "upnp"});
    auto& dns_group = add_group(page, "Private Servers",
        "Online Dreamcast games (Phantasy Star Online Ver. 2, Quake III Arena, Alien Front "
        "Online, ...) look their servers up through this DNS server. The default, "
        "dns.flyca.st, points them at the community revival servers. For a private server "
        "with its own DNS, enter its address here. Applies to the next game started.");
    auto* dns = Gtk::make_managed<Gtk::Entry>();
    dns->set_placeholder_text("dns.flyca.st (default)");
    dns->set_text(m_config.network.dns);
    dns->set_valign(Gtk::Align::CENTER);
    dns->set_width_chars(22);
    dns->signal_changed().connect([this, dns] {
        m_config.network.dns = dns->get_text();
        m_config.save();
    });
    add_row(dns_group, "DNS Server", "Host name or IP address", dns);
}

void SettingsWindow::build_emulation_page() {
    Page page = add_page("emulation", "Emulation", "media-playback-start-symbolic");
    if (!m_game) {
        auto& speed = add_group(page, "Speed Control");
        static const int kSpeeds[] = {25, 50, 75, 100, 125, 150, 200, 300, 400};
        std::vector<Glib::ustring> items;
        guint selected = 3;
        for (guint i = 0; i < std::size(kSpeeds); ++i) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%d%%  [%.0f FPS]", kSpeeds[i], 59.94 * kSpeeds[i] / 100.0);
            items.push_back(buf);
            if (kSpeeds[i] == m_config.display.speed) selected = i;
        }
        auto* drop = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(items));
        drop->set_selected(selected);
        drop->property_selected().signal_changed().connect([this, drop] {
            const guint sel = drop->get_selected();
            if (sel < std::size(kSpeeds)) {
                m_config.display.speed = kSpeeds[sel];
                m_config.save();
                signal_changed.emit();
            }
        });
        add_row(speed, "Emulation Speed",
                "How fast the console runs relative to a real one. Sound speeds up or slows "
                "down with it. Fast forward (Tab) is separate and runs as fast as possible.",
                drop);
    }
    add_engine_options(page, "Speed & Frame Pacing",
                       {"threaded_rendering", "auto_skip_frame", "frame_skipping",
                        "delay_frame_swapping", "detect_vsync_swap_interval"});
    add_engine_category(page, "Other", "performance");
}

void SettingsWindow::build_vmu_page() {
    Page page = add_page("vmu", "VMUs", "media-floppy-symbolic");
    add_engine_options(page, "Storage", {"per_content_vmus", "vmu_sound", "linked_vmu_storage"});
    for (int port = 1; port <= 4; ++port) {
        const std::string p = std::to_string(port);
        add_engine_options(page, std::string("Port ") + static_cast<char>('A' + port - 1),
                           {"device_port" + p + "_slot1", "device_port" + p + "_slot2",
                            "vmu" + p + "_screen_display", "vmu" + p + "_screen_position",
                            "vmu" + p + "_screen_size_mult", "vmu" + p + "_screen_opacity",
                            "vmu" + p + "_pixel_on_color", "vmu" + p + "_pixel_off_color"});
    }
    m_placed["show_vmu_screen_settings"] = true;   // a UI toggle of the engine's own menu
    add_engine_category(page, "Other", "vmu");
    add_engine_category(page, "Other Expansions", "expansions");
}

void SettingsWindow::build_graphics_page() {
    Page page = add_page("graphics", "Graphics", "video-display-symbolic");

    if (m_game) {
        add_engine_options(page, "Rendering", {"internal_resolution"});
    } else {
        // The global internal resolution is DreamStation's own setting (it
        // also drives the Settings menu and the resolution hotkeys).
        m_placed["internal_resolution"] = true;
        auto& group = add_group(page, "Upscaling & Widescreen");
        std::vector<Glib::ustring> items{"Automatic  —  match the window (4K display → 2160 lines)"};
        static const char* const kNotes[] = {" Native", "", "", "", " (for 4K)", "", "", " (for 8K)"};
        for (int scale = 1; scale <= 8; ++scale)
            items.push_back(std::to_string(scale) + "x" + kNotes[scale - 1] + "  —  " +
                            std::to_string(640 * scale) + "×" + std::to_string(480 * scale));
        auto* drop = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(items));
        drop->set_selected(std::min<u32>(m_config.graphics.internal_scale, 8));
        drop->property_selected().signal_changed().connect([this, drop] {
            m_config.graphics.internal_scale = drop->get_selected();
            m_config.save();
            signal_changed.emit();
        });
        add_row(group, "Internal Resolution",
                "The resolution the game's 3D scene is rendered at; the Dreamcast's own is "
                "640×480. Automatic picks the smallest size that covers the image on screen: "
                "2160 lines on a 4K display, which is exactly 3840×2160 with widescreen on.",
                drop);

        // One control for the two engine widescreen options.
        m_placed["widescreen_hack"]   = true;
        m_placed["widescreen_cheats"] = true;
        const auto on = [this](const char* key) {
            const auto it = m_config.engine.find(key);
            return it != m_config.engine.end() && it->second == "enabled";
        };
        auto* wide = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(
            {"Off (4:3)", "Widescreen Hack", "Widescreen Cheats"}));
        wide->set_selected(on("widescreen_cheats") ? 2u : on("widescreen_hack") ? 1u : 0u);
        wide->property_selected().signal_changed().connect([this, wide] {
            const guint sel = wide->get_selected();
            set_layer_value("widescreen_cheats",
                            sel == 2 ? std::optional<std::string>("enabled") : std::nullopt);
            set_layer_value("widescreen_hack",
                            sel == 1 ? std::optional<std::string>("enabled") : std::nullopt);
        });
        add_row(group, "Widescreen",
                "Hack: widens the 3D view of any game immediately; 2D overlays stay 4:3 and "
                "some games show objects popping in at the edges. Cheats: per-game codes that "
                "make the game itself render for 16:9, HUD included; only some games have "
                "them, and they apply when the game starts.", wide);
    }

    if (!m_game) {
        auto& display = add_group(page, "Display");
        const auto choice = [&](const char* title, const char* help,
                                std::vector<std::pair<const char*, const char*>> options,
                                std::string& target) {
            std::vector<Glib::ustring> items;
            guint selected = 0;
            for (std::size_t i = 0; i < options.size(); ++i) {
                items.push_back(options[i].second);
                if (target == options[i].first) selected = static_cast<guint>(i);
            }
            auto* drop = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(items));
            drop->set_selected(selected);
            std::string* dest = &target;
            drop->property_selected().signal_changed().connect([this, drop, options, dest] {
                const guint sel = drop->get_selected();
                if (sel < options.size()) {
                    *dest = options[sel].first;
                    m_config.save();
                    signal_changed.emit();
                }
            });
            add_row(display, title, help, drop);
        };
        choice("Aspect Ratio",
               "Auto follows the game: 4:3, or 16:9 when widescreen is on. Stretch fills the "
               "window regardless of shape.",
               {{"auto", "Auto (Game Native)"}, {"4:3", "4:3"}, {"16:9", "16:9"},
                {"stretch", "Stretch To Fill"}},
               m_config.display.aspect);
        choice("Scaling",
               "How the rendered image is resized to the window when no post-processing "
               "shader does it. Nearest keeps hard pixel edges; Bilinear smooths them.",
               {{"bilinear", "Bilinear (Smooth)"}, {"nearest", "Nearest-Neighbor"}},
               m_config.display.filter);
        add_switch(display, "Integer Scaling",
                   "Only scale the image by whole multiples of the console's 480 lines, "
                   "leaving borders if needed. Sharpest with Nearest-Neighbor at 1x.",
                   m_config.display.integer);
    }

    add_engine_options(page, "Rendering",
                       {"alpha_sorting", "oit_layers", "oit_abuffer_size", "texture_filtering",
                        "anisotropic_filtering", "mipmapping", "fog", "volume_modifier_enable",
                        "screen_rotation"});
    add_engine_options(page, "Enhancements",
                       {"widescreen_hack", "widescreen_cheats", "texupscale",
                        "texupscale_max_filtered_texture_size", "pvr2_filtering",
                        "fix_upscale_bleeding_edge", "native_depth_interpolation"});
    add_engine_options(page, "Accuracy", {"emulate_framebuffer", "enable_rttb"});
    add_engine_options(page, "Texture Replacement",
                       {"custom_textures", "preload_custom_textures", "dump_textures",
                        "dump_replaced_textures"});
    add_engine_category(page, "Other", "video");
}

void SettingsWindow::build_post_processing_page() {
    Page page = add_page("postprocessing", "Post-Processing", "applications-graphics-symbolic");
    m_post_box = page.box;
    rebuild_post_page();
}

// The chain editor: stages run top to bottom, each feeding the next.
void SettingsWindow::rebuild_post_page() {
    if (!m_post_box)
        return;
    while (auto* child = m_post_box->get_first_child())
        m_post_box->remove(*child);
    Page page{m_post_box};

    const auto changed = [this] {
        m_config.save();
        signal_post_chain.emit();
    };
    // Rebuilding destroys the widget whose handler is running: defer it.
    const auto changed_rebuild = [this, changed] {
        changed();
        Glib::signal_idle().connect_once([this] { rebuild_post_page(); });
    };

    auto& general = add_group(page, "Post-Processing",
        "Shaders applied to the final image, in order. RetroArch slang presets found on "
        "this machine are available; Dolphin/DuckStation-style .glsl files go in " +
        PostLibrary::user_dir().string() + ", and ReShade shader packs (their Shaders and "
        "Textures folders) in " + (PostLibrary::user_dir() / "reshade").string() + ".");
    {
        auto* sw = Gtk::make_managed<Gtk::Switch>();
        sw->set_active(m_config.post_enabled);
        sw->property_active().signal_changed().connect([this, sw, changed] {
            m_config.post_enabled = sw->get_active();
            changed();
        });
        auto* row = add_row(general, "Enable Post-Processing",
                            "Turn the whole chain on or off without losing its set-up.", sw);
        row->set_activatable_widget(sw);
    }

    auto& chain = add_group(page, "Post-Processing Chain");
    auto& stages = m_config.post_chain;
    for (std::size_t i = 0; i < stages.size(); ++i) {
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto* enabled = Gtk::make_managed<Gtk::Switch>();
        enabled->set_active(stages[i].enabled);
        enabled->set_valign(Gtk::Align::CENTER);
        enabled->property_active().signal_changed().connect([this, enabled, i, changed] {
            if (i < m_config.post_chain.size()) {
                m_config.post_chain[i].enabled = enabled->get_active();
                changed();
            }
        });
        const auto icon_button = [](const char* icon, const char* tip) {
            auto* b = Gtk::make_managed<Gtk::Button>();
            b->set_icon_name(icon);
            b->set_tooltip_text(tip);
            return b;
        };
        auto* up = icon_button("go-up-symbolic", "Move up");
        up->set_sensitive(i > 0);
        up->signal_clicked().connect([this, i, changed_rebuild] {
            if (i > 0 && i < m_config.post_chain.size()) {
                std::swap(m_config.post_chain[i], m_config.post_chain[i - 1]);
                changed_rebuild();
            }
        });
        auto* down = icon_button("go-down-symbolic", "Move down");
        down->set_sensitive(i + 1 < stages.size());
        down->signal_clicked().connect([this, i, changed_rebuild] {
            if (i + 1 < m_config.post_chain.size()) {
                std::swap(m_config.post_chain[i], m_config.post_chain[i + 1]);
                changed_rebuild();
            }
        });
        auto* remove = icon_button("list-remove-symbolic", "Remove from the chain");
        remove->signal_clicked().connect([this, i, changed_rebuild] {
            if (i < m_config.post_chain.size()) {
                m_config.post_chain.erase(m_config.post_chain.begin() +
                                          static_cast<std::ptrdiff_t>(i));
                changed_rebuild();
            }
        });
        box->append(*enabled);
        box->append(*up);
        box->append(*down);
        box->append(*remove);
        add_row(chain, std::to_string(i + 1) + ".  " + stages[i].shader,
                "Stage " + std::to_string(i + 1) + " of the chain. Its options are below.", box);
    }
    {
        auto* add = Gtk::make_managed<Gtk::Button>("Add Shader…");
        add->signal_clicked().connect([this] { open_shader_picker(); });
        add_row(chain, "Add Shader",
                "Append a shader to the end of the chain: a built-in one, a .glsl file from "
                "your shader folder, or a RetroArch slang preset.", add);
    }
    {
        auto* sw = Gtk::make_managed<Gtk::Switch>();
        sw->set_active(m_config.post_native_input);
        sw->property_active().signal_changed().connect([this, sw, changed] {
            m_config.post_native_input = sw->get_active();
            changed();
        });
        auto* row = add_row(general, "Give Shaders A 480-Line Image",
            "Shaders receive the game image reduced to the console's own 480 lines instead "
            "of the upscaled one. CRT shaders draw one scanline per input line, so they need "
            "this to look right at high internal resolutions; the picture is then only as "
            "sharp as a real Dreamcast's.", sw);
        row->set_activatable_widget(sw);
    }

    // Options of every stage, generated from the shaders' own declarations.
    for (std::size_t i = 0; i < stages.size(); ++i) {
        const auto options = PostLibrary::options(stages[i].shader);
        if (options.empty())
            continue;
        auto& group = add_group(page, std::to_string(i + 1) + ".  " + stages[i].shader);
        for (const PostOption& opt : options) {
            const auto it = stages[i].options.find(opt.name);
            const float value = it != stages[i].options.end() ? it->second : opt.def;
            const std::string name = opt.name;
            const float def = opt.def;
            const auto store = [this, i, name, def](float v) {
                if (i >= m_config.post_chain.size())
                    return;
                auto& map = m_config.post_chain[i].options;
                if (v == def) map.erase(name);
                else          map[name] = v;
                m_config.save();
                signal_post_option.emit(i, name, v);
            };
            if (!opt.items.empty()) {
                std::vector<Glib::ustring> names(opt.items.begin(), opt.items.end());
                auto* drop = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(names));
                drop->set_valign(Gtk::Align::CENTER);
                drop->set_selected(static_cast<guint>(std::clamp(
                    static_cast<int>(std::lround(value - opt.min)), 0,
                    static_cast<int>(opt.items.size()) - 1)));
                const float base = opt.min;
                drop->property_selected().signal_changed().connect([drop, store, base] {
                    store(base + static_cast<float>(drop->get_selected()));
                });
                add_row(group, opt.label, {}, drop);
                continue;
            }
            if (opt.type == PostOption::Type::Bool) {
                auto* sw = Gtk::make_managed<Gtk::Switch>();
                sw->set_active(value != 0.f);
                sw->property_active().signal_changed().connect(
                    [sw, store] { store(sw->get_active() ? 1.f : 0.f); });
                add_row(group, opt.label, {}, sw);
                continue;
            }
            auto* box   = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            auto* scale = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::HORIZONTAL);
            scale->set_range(opt.min, opt.max);
            scale->set_increments(opt.step, opt.step * 5);
            scale->set_value(value);
            scale->set_size_request(260, -1);
            scale->set_draw_value(false);
            auto* readout = Gtk::make_managed<Gtk::Label>();
            readout->set_width_chars(7);
            const bool is_int = opt.type == PostOption::Type::Int;
            const auto show = [readout, is_int](double v) {
                char buf[24];
                std::snprintf(buf, sizeof(buf), is_int ? "%.0f" : "%.3g", v);
                readout->set_text(buf);
            };
            show(value);
            scale->signal_value_changed().connect([scale, store, show, is_int] {
                double v = scale->get_value();
                if (is_int) v = std::round(v);
                show(v);
                store(static_cast<float>(v));
            });
            box->append(*scale);
            box->append(*readout);
            add_row(group, opt.label,
                    "Default: " + std::to_string(opt.def) + ". Changes apply live.", box);
        }
    }
}

void SettingsWindow::build_osd_page() {
    Page page = add_page("osd", "On-Screen Display", "video-display-symbolic");
    auto& o = m_config.osd;

    auto& display = add_group(page, "Display");
    {
        auto* box   = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto* scale = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::HORIZONTAL);
        scale->set_range(50, 400);
        scale->set_increments(10, 50);
        scale->set_value(o.scale);
        scale->set_size_request(260, -1);
        scale->set_draw_value(false);
        auto* readout = Gtk::make_managed<Gtk::Label>(std::to_string(o.scale) + "%");
        readout->set_width_chars(6);
        scale->signal_value_changed().connect([this, scale, readout] {
            m_config.osd.scale = static_cast<int>(std::lround(scale->get_value() / 10.0) * 10);
            readout->set_text(std::to_string(m_config.osd.scale) + "%");
            m_config.save();
            signal_changed.emit();
        });
        box->append(*scale);
        box->append(*readout);
        add_row(display, "Display Scale", "Size of everything the on-screen display draws.", box);
    }

    auto& messages = add_group(page, "Messages");
    add_switch(messages, "Show Messages",
               "Show notifications over the game: state saved, screenshot taken, setting "
               "changed and so on.", o.messages);
    {
        static const std::vector<std::pair<const char*, const char*>> kCorners = {
            {"top-left", "Top Left"}, {"top-center", "Top Center"}, {"top-right", "Top Right"},
            {"bottom-left", "Bottom Left"}, {"bottom-center", "Bottom Center"},
            {"bottom-right", "Bottom Right"}};
        std::vector<Glib::ustring> items;
        guint selected = 0;
        for (std::size_t i = 0; i < kCorners.size(); ++i) {
            items.push_back(kCorners[i].second);
            if (o.position == kCorners[i].first) selected = static_cast<guint>(i);
        }
        auto* drop = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(items));
        drop->set_selected(selected);
        drop->property_selected().signal_changed().connect([this, drop] {
            const guint sel = drop->get_selected();
            if (sel < kCorners.size()) {
                m_config.osd.position = kCorners[sel].first;
                m_config.save();
                signal_changed.emit();
            }
        });
        add_row(messages, "Display Location", "Where messages appear.", drop);
    }

    auto& overlays = add_group(page, "Overlays");
    add_switch(overlays, "Show FPS",
               "The number of new images the game drew in the last second.", o.show_fps);
    add_switch(overlays, "Show Emulation Speed",
               "Emulated video frames per second and the speed relative to a real console.",
               o.show_speed);
    add_switch(overlays, "Show Resolution",
               "The resolution the game is being rendered at.", o.show_resolution);
    add_switch(overlays, "Show Frame Times",
               "Average and worst time taken to emulate a frame over the last second. A "
               "frame must fit in 16.7 ms to hold full speed.", o.show_frame_times);
}

void SettingsWindow::build_audio_page() {
    Page page = add_page("audio", "Audio", "audio-speakers-symbolic");
    const auto changed = [this] {
        m_config.save();
        signal_changed.emit();
    };

    auto& controls = add_group(page, "Controls");
    {
        auto* box   = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto* scale = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::HORIZONTAL);
        scale->set_range(0, 200);
        scale->set_increments(5, 25);
        scale->set_value(m_config.audio.volume);
        scale->set_size_request(260, -1);
        scale->set_draw_value(false);
        auto* readout = Gtk::make_managed<Gtk::Label>(std::to_string(m_config.audio.volume) + "%");
        readout->set_width_chars(6);
        scale->signal_value_changed().connect([this, scale, readout, changed] {
            m_config.audio.volume = static_cast<int>(std::lround(scale->get_value()));
            readout->set_text(std::to_string(m_config.audio.volume) + "%");
            changed();
        });
        box->append(*scale);
        box->append(*readout);
        add_row(controls, "Output Volume",
                "Volume of the game's sound. Above 100% amplifies and may distort.", box);
    }
    add_switch(controls, "Mute All Sound", "Silence the game without changing the volume.",
               m_config.audio.muted);

    auto& output = add_group(page, "Configuration");
    {
        // Playback devices as the sound system reports them.
        std::vector<std::string> names{""};
        std::vector<Glib::ustring> items{"System Default"};
        int count = 0;
        if (SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count)) {
            for (int i = 0; i < count; ++i)
                if (const char* name = SDL_GetAudioDeviceName(ids[i])) {
                    names.emplace_back(name);
                    items.emplace_back(name);
                }
            SDL_free(ids);
        }
        guint selected = 0;
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == m_config.audio.device) selected = static_cast<guint>(i);
        auto* drop = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(items));
        drop->set_selected(selected);
        drop->property_selected().signal_changed().connect([this, drop, names, changed] {
            const guint sel = drop->get_selected();
            if (sel < names.size()) {
                m_config.audio.device = names[sel];
                changed();
            }
        });
        add_row(output, "Output Device",
                "Where the sound goes. System Default follows your desktop's choice.", drop);
    }
    {
        auto* box   = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto* scale = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::HORIZONTAL);
        scale->set_range(20, 300);
        scale->set_increments(5, 20);
        scale->set_value(m_config.audio.buffer_ms);
        scale->set_size_request(260, -1);
        scale->set_draw_value(false);
        auto* readout = Gtk::make_managed<Gtk::Label>(std::to_string(m_config.audio.buffer_ms) + " ms");
        readout->set_width_chars(7);
        scale->signal_value_changed().connect([this, scale, readout, changed] {
            m_config.audio.buffer_ms = static_cast<int>(std::lround(scale->get_value()));
            readout->set_text(std::to_string(m_config.audio.buffer_ms) + " ms");
            changed();
        });
        box->append(*scale);
        box->append(*readout);
        add_row(output, "Buffer Size",
                "How much sound is kept queued ahead of playback. Lower means less delay "
                "between action and sound; raise it if the sound crackles.", box);
    }
}

// A searchable list of every available shader (there can be thousands of
// slang presets), like DuckStation's "Select Shader" dialog.
void SettingsWindow::open_shader_picker() {
    auto* win = new Gtk::Window();
    win->set_title("Select Shader");
    win->set_default_size(640, 620);
    win->set_transient_for(*this);
    win->set_modal(true);
    win->add_css_class("ds-settings");

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_margin(12);
    auto* search = Gtk::make_managed<Gtk::SearchEntry>();
    search->set_placeholder_text("Search shaders…");
    auto* kind = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(
        {"All", "Built-in and GLSL files", "Slang presets", "ReShade effects"}));
    auto* top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    search->set_hexpand(true);
    top->append(*search);
    top->append(*kind);
    box->append(*top);

    auto all   = std::make_shared<std::vector<std::string>>(PostLibrary::available());
    auto shown = std::make_shared<std::vector<std::string>>();
    auto* list = Gtk::make_managed<Gtk::ListBox>();
    list->set_selection_mode(Gtk::SelectionMode::SINGLE);
    list->add_css_class("boxed-list");
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_child(*list);
    scroll->set_vexpand(true);
    box->append(*scroll);
    auto* count = Gtk::make_managed<Gtk::Label>();
    count->set_xalign(0.f);
    count->add_css_class("dim-label");
    box->append(*count);

    const auto refill = [all, shown, list, search, kind, count] {
        while (auto* child = list->get_first_child())
            list->remove(*child);
        shown->clear();
        std::string query = search->get_text();
        std::transform(query.begin(), query.end(), query.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const guint which = kind->get_selected();
        std::size_t matches = 0;
        for (const auto& name : *all) {
            const bool slang   = name.rfind("slang/", 0) == 0;
            const bool reshade = name.rfind("reshade/", 0) == 0;
            if ((which == 1 && (slang || reshade)) || (which == 2 && !slang) ||
                (which == 3 && !reshade))
                continue;
            std::string low = name;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!query.empty() && low.find(query) == std::string::npos)
                continue;
            ++matches;
            if (shown->size() >= 400)      // keep the list light; narrow by typing
                continue;
            auto* label = Gtk::make_managed<Gtk::Label>(name);
            label->set_xalign(0.f);
            label->set_margin(6);
            label->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            list->append(*label);
            shown->push_back(name);
        }
        count->set_text(std::to_string(matches) + " shaders" +
                        (matches > shown->size() ? "  —  showing the first 400, type to narrow" : ""));
    };
    search->signal_search_changed().connect(refill);
    kind->property_selected().signal_changed().connect(refill);
    refill();

    const auto choose = [this, win, list, shown] {
        const Gtk::ListBoxRow* row = list->get_selected_row();
        if (!row)
            return;
        const auto index = static_cast<std::size_t>(row->get_index());
        if (index >= shown->size())
            return;
        m_config.post_chain.push_back({(*shown)[index], true, {}});
        m_config.save();
        signal_post_chain.emit();
        win->close();
        Glib::signal_idle().connect_once([this] { rebuild_post_page(); });
    };
    list->signal_row_activated().connect([choose](Gtk::ListBoxRow*) { choose(); });

    auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    buttons->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    cancel->signal_clicked().connect([win] { win->close(); });
    auto* add = Gtk::make_managed<Gtk::Button>("Add");
    add->signal_clicked().connect(choose);
    buttons->append(*cancel);
    buttons->append(*add);
    box->append(*buttons);

    win->set_child(*box);
    win->signal_hide().connect([win] { delete win; });
    win->present();
    search->grab_focus();
}

void SettingsWindow::build_controllers_page() {
    Page page = add_page("controllers", "Input Options", "input-gaming-symbolic");
    if (!m_game) {
        auto& vib = add_group(page, "Vibration");
        add_switch(vib, "Enable Vibration",
                   "Pass the Dreamcast rumble pack's vibration on to your controller. The "
                   "port needs a rumble pack in one of its slots (Settings → VMUs).",
                   m_config.behavior.rumble);
    }
    add_engine_options(page, "Analog Input",
                       {"analog_stick_deadzone", "trigger_deadzone", "digital_triggers"});
    add_engine_options(page, "Light Guns",
                       {"lightgun_crosshair_size_scaling", "lightgun1_crosshair",
                        "lightgun2_crosshair", "lightgun3_crosshair", "lightgun4_crosshair"});
    m_placed["show_lightgun_settings"] = true;
    add_engine_category(page, "Other", "input");
}

void SettingsWindow::build_advanced_page() {
    Page page = add_page("advanced", "Advanced", "applications-engineering-symbolic");
    add_engine_category(page, "Hacks", "hacks");
    // Anything the engine reports that no page above has shown.
    std::vector<std::string> rest;
    for (const auto& o : m_options)
        if (!m_placed.count(o.key))
            rest.push_back(o.key);
    add_engine_options(page, "Uncategorised Engine Options", rest);

    if (!m_game) {
        auto& group = add_group(page, "Data");
        auto* lbl = Gtk::make_managed<Gtk::Label>(Config::data_dir().string());
        lbl->set_selectable(true);
        lbl->add_css_class("dim-label");
        add_row(group, "Data Directory",
                "Save states, screenshots, VMUs, per-game settings and covers live here.", lbl);
    }
}

} // namespace ds::frontend
