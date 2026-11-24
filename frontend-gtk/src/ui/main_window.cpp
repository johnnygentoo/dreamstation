// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.hpp"
#include "library_page.hpp"
#include "render_widget.hpp"
#include "settings_window.hpp"
#include "cover_downloader.hpp"
#include "controllers_window.hpp"
#include "pause_menu.hpp"
#include "vmu_manager.hpp"
#include "vmu_player.hpp"
#include "vmu_window.hpp"
#include "netplay_dialog.hpp"
#include "alert.hpp"
#include "../vmu.hpp"
#include "osd_layer.hpp"
#include "../host/pad_input.hpp"
#include "../config.hpp"
#include <dreamstation/common/log.hpp>

#include <gdk/gdkkeysyms.h>
#include <giomm/subprocess.h>
#include <algorithm>
#include <set>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ds::frontend {

// ── Construction ──────────────────────────────────────────────────────────

MainWindow::MainWindow(Config& cfg) : m_config(cfg) {
    set_title("DreamStation");
    set_default_size(m_config.window.width, m_config.window.height);
    if (m_config.window.maximized) maximize();

    m_hotkeys.rebuild(m_config.hotkeys, m_config.hotkey_pads);
    m_stats.load();
    apply_input_config();
    m_load_menu = Gio::Menu::create();
    m_save_menu = Gio::Menu::create();
    m_disc_menu = Gio::Menu::create();

    setup_actions();
    build_menu();
    build_ui();
    rebuild_state_menus();
    rebuild_disc_menu();
    apply_post_chain();
    apply_audio_config();
    apply_display_config();
    apply_osd_config();

    // Gamepad hotkeys are polled: the pad is not a GTK input device.
    m_pad_hotkey_timer = Glib::signal_timeout().connect(
        sigc::mem_fun(*this, &MainWindow::poll_pad_hotkeys), 20);

    // Drop a disc image (or a save state) on the window to run it.
    auto drop = Gtk::DropTarget::create(GDK_TYPE_FILE_LIST, Gdk::DragAction::COPY);
    drop->signal_drop().connect([this](const Glib::ValueBase& value, double, double) {
        GSList* files = static_cast<GSList*>(g_value_get_boxed(value.gobj()));
        if (!files || !files->data)
            return false;
        char* raw = g_file_get_path(G_FILE(files->data));
        if (!raw)
            return false;
        const std::filesystem::path path = raw;
        g_free(raw);
        if (path.extension() == ".sav") {
            if (m_emu_running)
                show_toast(load_state_file(path) ? "State loaded" : "Could not load state");
            else
                show_toast("Start the game first, then drop the state");
        } else {
            launch(path);
        }
        return true;
    }, false);
    add_controller(drop);

    // Closing the window while a game runs goes through the same confirm /
    // save-for-resume path as Close Game.
    signal_close_request().connect([this]() -> bool {
        if (!m_emu_running || m_closing)
            return false;
        request_close_game([this] { m_closing = true; close(); });
        return true;
    }, false);

    // Optional: pause while another application has focus.
    property_is_active().signal_changed().connect([this] {
        if (!m_emu_running || !m_config.behavior.pause_on_focus_loss)
            return;
        if (!is_active() && !m_render_widget->paused()) {
            m_paused_by_focus = true;
            set_paused(true);
        } else if (is_active() && m_paused_by_focus) {
            m_paused_by_focus = false;
            set_paused(false);
        }
    });

    // Scan game library
    if (!m_config.paths.game_dirs.empty())
        rescan_library();
    // The VMU window comes back if it was open when the program was closed.
    if (m_config.iface.vmu_window)
        Glib::signal_idle().connect_once([this] { set_vmu_window_visible(true); });

    run_test_script();
}

MainWindow::~MainWindow() {
    m_fps_timer.disconnect();
    m_auto_res_timer.disconnect();
    m_pad_hotkey_timer.disconnect();
    m_cover_refresh.disconnect();
    m_rescan_timer.disconnect();
    m_vmu_timer.disconnect();
    m_vmu_window.reset();            // saves a mini-game being played
    m_dir_monitors.clear();
    m_rescan_after_game = false;     // no rescan from stop_emulation() below
    m_load_menu.reset();   // stop_emulation() must not touch menus now
    stop_emulation();

    // Save window geometry
    int w = 0, h = 0;
    get_default_size(w, h);
    m_config.window.width     = w;
    m_config.window.height    = h;
    m_config.window.maximized = is_maximized();
    m_config.save();
}

// ── Actions ───────────────────────────────────────────────────────────────

void MainWindow::setup_actions() {
    auto grp = Gio::SimpleActionGroup::create();
    m_action_group = grp;

    grp->add_action("open-file",  sigc::mem_fun(*this, &MainWindow::on_open_disc));
    grp->add_action("start-bios", sigc::mem_fun(*this, &MainWindow::on_start_bios));
    grp->add_action("netplay",    [this] { open_netplay_dialog({}); });
    grp->add_action("reset",      sigc::mem_fun(*this, &MainWindow::on_reset));
    grp->add_action("power-off",  sigc::mem_fun(*this, &MainWindow::on_power_off));
    grp->add_action("open-data-dir", sigc::mem_fun(*this, &MainWindow::on_open_data_dir));
    grp->add_action("about",      sigc::mem_fun(*this, &MainWindow::on_about));
    grp->add_action("settings",   [this]{ open_settings("interface"); });
    grp->add_action("game-properties", [this]{ open_game_properties(m_current_game_path); });
    for (const char* page : {"gamelist", "console", "emulation", "vmu", "postprocessing",
                             "controllers", "advanced"})
        grp->add_action(std::string(page) + "-settings", [this, page]{ open_settings(page); });
    grp->add_action("bios-settings",     [this]{ open_settings("bios");      });
    grp->add_action("graphics-settings", [this]{ open_settings("graphics");  });
    grp->add_action("interface-settings",[this]{ open_settings("interface"); });
    grp->add_action("rescan-library",    [this]{ rescan_library(); });

    // Stubs — toast feedback
    const auto stub = [this, &grp](const char* name) {
        grp->add_action(name, [this, name]{
            show_toast(Glib::ustring(name) + " — not implemented yet");
        });
    };
    grp->add_action_with_parameter("load-slot", Glib::VARIANT_TYPE_STRING,
        [this](const Glib::VariantBase& v) {
            on_state_action(Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(v).get(), false);
        });
    grp->add_action_with_parameter("save-slot", Glib::VARIANT_TYPE_STRING,
        [this](const Glib::VariantBase& v) {
            on_state_action(Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(v).get(), true);
        });
    grp->add_action("undo-load-state", sigc::mem_fun(*this, &MainWindow::undo_load_state));
    grp->add_action("screenshot",      sigc::mem_fun(*this, &MainWindow::take_screenshot));
    grp->add_action("power-off-nosave", [this] { if (m_emu_running) close_game_now(false); });
    grp->add_action("quit", [this] { close(); });
    grp->add_action_with_parameter("change-disc", Glib::VARIANT_TYPE_STRING,
        [this](const Glib::VariantBase& v) {
            change_disc(Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(v).get());
        });
    m_act_merge = Gio::SimpleAction::create_bool("merge-discs", m_config.library.merge_discs);
    m_act_merge->signal_change_state().connect([this](const Glib::VariantBase& v) {
        m_act_merge->set_state(v);
        m_config.library.merge_discs = Glib::VariantBase::cast_dynamic<Glib::Variant<bool>>(v).get();
        m_config.save();
        m_library_page->refresh();
    });
    grp->add_action(m_act_merge);
    m_act_box_covers = Gio::SimpleAction::create_bool("box-covers", m_config.library.box_covers);
    m_act_box_covers->signal_change_state().connect([this](const Glib::VariantBase& v) {
        m_act_box_covers->set_state(v);
        m_config.library.box_covers = Glib::VariantBase::cast_dynamic<Glib::Variant<bool>>(v).get();
        m_config.save();
        m_library_page->refresh();
    });
    grp->add_action(m_act_box_covers);
    grp->add_action("audio-settings", [this] { open_settings("audio"); });
    stub("start-disc");
    grp->add_action("vmu-editor", [this] {
        // Rebuilt each time so it always shows what is on disk now.
        m_vmu_manager = std::make_unique<VmuManager>(
            std::vector<std::filesystem::path>{Config::data_dir() / "system" / "dc",
                                               Config::data_dir() / "saves"},
            [this] { return m_emu_running; },
            [this](const std::filesystem::path& card) { play_vmu_game(card); });
        m_vmu_manager->set_transient_for(*this);
        m_vmu_manager->present();
    });
    grp->add_action("vmu-play", [this] {
        auto dlg = Gtk::FileDialog::create();
        dlg->set_title("Play VMU Mini-Game");
        auto filters = Gio::ListStore<Gtk::FileFilter>::create();
        auto games = Gtk::FileFilter::create();
        games->set_name("VMU mini-games and cards (VMS, DCI, BIN, VMU, DCM)");
        for (const char* p : {"*.vms", "*.VMS", "*.vmi", "*.VMI", "*.dci", "*.DCI", "*.bin", "*.BIN",
                              "*.vmu", "*.VMU", "*.dcm", "*.DCM"})
            games->add_pattern(p);
        filters->append(games);
        dlg->set_filters(filters);
        dlg->set_initial_folder(Gio::File::create_for_path((Config::data_dir() / "system" / "dc").string()));
        dlg->open(*this, [this, dlg](const Glib::RefPtr<Gio::AsyncResult>& res) {
            try {
                play_vmu_game(dlg->open_finish(res)->get_path());
            } catch (...) {}
        });
    });
    grp->add_action_with_parameter("vmu-play-file", Glib::VARIANT_TYPE_STRING,
        [this](const Glib::VariantBase& v) {
            play_vmu_game(Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(v).get().raw());
        });
    stub("cheat-manager");
    grp->add_action("cover-downloader", [this] {
        ensure_cover_downloader();
        m_cover_downloader->present();
    });
    m_act_auto_covers = Gio::SimpleAction::create_bool("auto-covers", m_config.library.auto_covers);
    m_act_auto_covers->signal_change_state().connect([this](const Glib::VariantBase& v) {
        m_act_auto_covers->set_state(v);
        m_config.library.auto_covers = Glib::VariantBase::cast_dynamic<Glib::Variant<bool>>(v).get();
        m_config.save();
        if (m_config.library.auto_covers)
            fetch_missing_covers();
    });
    grp->add_action(m_act_auto_covers);
    m_act_vmu_overlay = Gio::SimpleAction::create_bool("vmu-overlay", m_config.osd.vmu_screen);
    m_act_vmu_overlay->signal_change_state().connect([this](const Glib::VariantBase& v) {
        m_act_vmu_overlay->set_state(v);
        m_config.osd.vmu_screen = Glib::VariantBase::cast_dynamic<Glib::Variant<bool>>(v).get();
        m_config.save();
        m_vmu_serial.fill(0);            // draw the current screen again
        if (!m_config.osd.vmu_screen && m_osd)
            m_osd->set_vmu_screen(nullptr);
    });
    grp->add_action(m_act_vmu_overlay);
    m_act_vmu_window = Gio::SimpleAction::create_bool("vmu-window", false);
    m_act_vmu_window->signal_change_state().connect([this](const Glib::VariantBase& v) {
        set_vmu_window_visible(Glib::VariantBase::cast_dynamic<Glib::Variant<bool>>(v).get());
    });
    grp->add_action(m_act_vmu_window);
    grp->add_action("controllers", [this] { open_controllers("port0"); });
    grp->add_action("hotkeys",     [this] { open_controllers("hotkeys"); });
    grp->add_action("view-grid", [this] { m_views.set_visible_child_name("library"); m_library_page->set_view("grid"); });
    grp->add_action("view-list", [this] { m_views.set_visible_child_name("library"); m_library_page->set_view("list"); });
    grp->add_action("zoom-in",   [this] { m_library_page->zoom(+1); });
    grp->add_action("zoom-out",  [this] { m_library_page->zoom(-1); });
    stub("docs");         stub("github");

    // Stateful bool actions
    m_act_pause = Gio::SimpleAction::create_bool("pause", false);
    m_act_pause->signal_change_state().connect([this](const Glib::VariantBase& v) {
        bool b = Glib::VariantBase::cast_dynamic<Glib::Variant<bool>>(v).get();
        m_act_pause->set_state(v);
        on_pause_toggle(b);
    });
    grp->add_action(m_act_pause);

    m_act_fullscreen = Gio::SimpleAction::create_bool("fullscreen", false);
    m_act_fullscreen->signal_change_state().connect([this](const Glib::VariantBase& v) {
        bool b = Glib::VariantBase::cast_dynamic<Glib::Variant<bool>>(v).get();
        m_act_fullscreen->set_state(v);
        on_fullscreen_toggle(b);
    });
    grp->add_action(m_act_fullscreen);

    m_act_toolbar = Gio::SimpleAction::create_bool(
        "show-toolbar", m_config.iface.show_toolbar);
    m_act_toolbar->signal_change_state().connect([this](const Glib::VariantBase& v) {
        bool b = Glib::VariantBase::cast_dynamic<Glib::Variant<bool>>(v).get();
        m_act_toolbar->set_state(v);
        on_show_toolbar(b);
    });
    grp->add_action(m_act_toolbar);

    m_act_statusbar = Gio::SimpleAction::create_bool(
        "show-status-bar", m_config.iface.show_status_bar);
    m_act_statusbar->signal_change_state().connect([this](const Glib::VariantBase& v) {
        bool b = Glib::VariantBase::cast_dynamic<Glib::Variant<bool>>(v).get();
        m_act_statusbar->set_state(v);
        on_show_status_bar(b);
    });
    grp->add_action(m_act_statusbar);

    m_act_search = Gio::SimpleAction::create_bool("search", false);
    m_act_search->signal_change_state().connect([this](const Glib::VariantBase& v) {
        bool b = Glib::VariantBase::cast_dynamic<Glib::Variant<bool>>(v).get();
        m_act_search->set_state(v);
        on_search_toggle(b);
    });
    grp->add_action(m_act_search);

    // Post-processing chain on/off (the chain itself is edited in Settings).
    m_act_post = Gio::SimpleAction::create_bool("post-processing", m_config.post_enabled);
    m_act_post->signal_change_state().connect([this](const Glib::VariantBase& v) {
        m_act_post->set_state(v);
        m_config.post_enabled = Glib::VariantBase::cast_dynamic<Glib::Variant<bool>>(v).get();
        m_config.save();
        apply_post_chain();
    });
    grp->add_action(m_act_post);

    // Internal resolution scale — radio action, applies live.
    m_act_resolution = Gio::SimpleAction::create_radio_string(
        "resolution", std::to_string(std::min<u32>(m_config.graphics.internal_scale, 8)));
    m_act_resolution->signal_change_state().connect([this](const Glib::VariantBase& v) {
        const auto s = Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(v).get();
        m_act_resolution->set_state(v);
        on_resolution_changed(static_cast<u32>(std::stoul(std::string(s))));
    });
    grp->add_action(m_act_resolution);

    // Widescreen: off, the engine's generic hack, or per-game cheat codes.
    m_act_widescreen = Gio::SimpleAction::create_radio_string("widescreen", widescreen_mode());
    m_act_widescreen->signal_change_state().connect([this](const Glib::VariantBase& v) {
        m_act_widescreen->set_state(v);
        on_widescreen_changed(Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(v).get());
    });
    grp->add_action(m_act_widescreen);

    insert_action_group("win", grp);
    update_sensitivity();
}

// ── Menu ──────────────────────────────────────────────────────────────────

// Each top-level menu is exposed as a labeled MenuButton in the header bar.
// build_menu() builds the Gio::Menu models; build_ui() attaches them.
// Keeping the models alive is handled by the MenuButton widgets (which hold
// a Glib::RefPtr to their model), so nothing needs to be stored as a member.
static Glib::RefPtr<Gio::Menu> make_system_menu(const Glib::RefPtr<Gio::Menu>& load_menu,
                                                const Glib::RefPtr<Gio::Menu>& save_menu,
                                                const Glib::RefPtr<Gio::Menu>& disc_menu) {
    auto m = Gio::Menu::create();

    auto start = Gio::Menu::create();
    start->append("Start File…",       "win.open-file");
    start->append("Start BIOS",        "win.start-bios");
    start->append("Online Play…",      "win.netplay");
    m->append_section({}, start);

    auto emu = Gio::Menu::create();
    emu->append("Close Game",                "win.power-off");
    emu->append("Close Game Without Saving", "win.power-off-nosave");
    emu->append("Restart Game",              "win.reset");
    emu->append("Pause",                     "win.pause");
    emu->append_submenu("Change Disc", disc_menu);
    emu->append("Screenshot",          "win.screenshot");
    m->append_section({}, emu);

    auto state = Gio::Menu::create();
    state->append_submenu("Load State", load_menu);
    state->append_submenu("Save State", save_menu);
    m->append_section({}, state);

    auto quit = Gio::Menu::create();
    quit->append("Exit",               "win.quit");
    m->append_section({}, quit);

    return m;
}

static Glib::RefPtr<Gio::Menu> make_settings_menu() {
    auto m = Gio::Menu::create();

    // Internal resolution (640x480 native), like DuckStation's resolution scale.
    auto res = Gio::Menu::create();
    static const char* const kNotes[] = {"Native", "", "", "", "4K", "", "", "8K"};
    res->append("Automatic  (match the window)", "win.resolution::0");
    for (int scale = 1; scale <= 8; ++scale) {
        std::string label = std::to_string(scale) + "x  (" + std::to_string(640 * scale) +
                            "×" + std::to_string(480 * scale) + ")";
        if (kNotes[scale - 1][0])
            label += std::string("  ") + kNotes[scale - 1];
        res->append(label, "win.resolution::" + std::to_string(scale));
    }
    m->append_submenu("Internal Resolution", res);

    auto wide = Gio::Menu::create();
    wide->append("Off  (4:3)",                              "win.widescreen::off");
    wide->append("Widescreen Hack  (any game, live)",       "win.widescreen::hack");
    wide->append("Widescreen Cheats  (per game, on restart)", "win.widescreen::cheats");
    m->append_submenu("Widescreen", wide);

    auto props = Gio::Menu::create();
    props->append("Game Properties…",   "win.game-properties");
    m->append_section({}, props);

    auto pages = Gio::Menu::create();
    pages->append("Interface…",         "win.interface-settings");
    pages->append("Game List…",         "win.gamelist-settings");
    pages->append("BIOS…",              "win.bios-settings");
    pages->append("Console…",           "win.console-settings");
    pages->append("Emulation…",         "win.emulation-settings");
    pages->append("VMUs…",              "win.vmu-settings");
    pages->append("Graphics…",          "win.graphics-settings");
    pages->append("Post-Processing…",   "win.postprocessing-settings");
    pages->append("Audio…",             "win.audio-settings");
    pages->append("Input Options…",     "win.controllers-settings");
    pages->append("Advanced…",          "win.advanced-settings");
    m->append_section({}, pages);

    auto input = Gio::Menu::create();
    input->append("Controllers…",       "win.controllers");
    input->append("Hotkeys…",           "win.hotkeys");
    m->append_section({}, input);
    return m;
}

static Glib::RefPtr<Gio::Menu> make_view_menu() {
    auto m = Gio::Menu::create();
    auto list = Gio::Menu::create();
    list->append("Game List",           "win.view-list");
    list->append("Game Grid",           "win.view-grid");
    list->append("Zoom In",             "win.zoom-in");
    list->append("Zoom Out",            "win.zoom-out");
    list->append("Merge Multi-Disc Games", "win.merge-discs");
    list->append("3D Box Covers",       "win.box-covers");
    m->append_section({}, list);
    m->append("Search",                 "win.search");
    m->append("Toolbar",                "win.show-toolbar");
    m->append("Status Bar",             "win.show-status-bar");
    m->append("Fullscreen",             "win.fullscreen");

    m->append("Post-Processing",        "win.post-processing");
    auto vmu = Gio::Menu::create();
    vmu->append("VMU Window",           "win.vmu-window");
    vmu->append("VMU Screen Over Game", "win.vmu-overlay");
    m->append_section({}, vmu);
    return m;
}

static Glib::RefPtr<Gio::Menu> make_tools_menu() {
    auto m = Gio::Menu::create();
    m->append("Rescan Library",         "win.rescan-library");
    m->append("Open Data Directory",    "win.open-data-dir");
    m->append("VMU Manager…",           "win.vmu-editor");
    m->append("Play VMU Mini-Game…",    "win.vmu-play");
    m->append("Cheat Manager…",         "win.cheat-manager");
    m->append("Cover Downloader…",      "win.cover-downloader");
    m->append("Download Missing Covers Automatically", "win.auto-covers");
    return m;
}

static Glib::RefPtr<Gio::Menu> make_help_menu() {
    auto m = Gio::Menu::create();
    m->append("Documentation",          "win.docs");
    m->append("GitHub",                 "win.github");
    m->append("About DreamStation",     "win.about");
    return m;
}

void MainWindow::build_menu() {
    // Models are built per-section and attached by build_ui().
    // Nothing to do here — exists as a hook for future precomputation.
}

// ── UI ────────────────────────────────────────────────────────────────────

void MainWindow::build_ui() {
    // ── Toast overlay wraps everything ───────────────────────────────────
    set_content(&m_toasts);
    m_toasts.set_child(&m_root);

    // ── Header ────────────────────────────────────────────────────────────
    m_view_title.set_title("DreamStation");
    m_view_title.set_stack(&m_views);
    m_header.set_title_widget(&m_view_title);
    // Minimise, maximise and close are left to the desktop.
    m_header.set_show_start_title_buttons(false);
    m_header.set_show_end_title_buttons(false);

    // Helper: create a labeled menu button for the header bar
    const auto make_menu_btn = [](const char* label,
                                   Glib::RefPtr<Gio::Menu> model) {
        auto* btn = Gtk::make_managed<Gtk::MenuButton>();
        btn->set_label(label);
        btn->set_menu_model(model);
        btn->add_css_class("flat");
        btn->add_css_class("ds-header-menu-btn");
        return btn;
    };

    // Left side: open-disc shortcut, then System / Settings menus
    auto* open_btn = Gtk::make_managed<Gtk::Button>();
    open_btn->set_icon_name("document-open-symbolic");
    open_btn->set_tooltip_text("Open Disc Image… (Ctrl+O)");
    open_btn->set_action_name("win.open-file");
    m_header.pack_start(open_btn);

    m_header.pack_start(make_menu_btn("System",   make_system_menu(m_load_menu, m_save_menu, m_disc_menu)));
    m_header.pack_start(make_menu_btn("Settings", make_settings_menu()));

    // Right side: View / Tools / Help menus, then search toggle
    m_header.pack_end(make_menu_btn("Help",    make_help_menu()));
    m_header.pack_end(make_menu_btn("Tools",   make_tools_menu()));
    m_header.pack_end(make_menu_btn("View",    make_view_menu()));

    auto* search_btn = Gtk::make_managed<Gtk::ToggleButton>();
    search_btn->set_icon_name("system-search-symbolic");
    search_btn->set_tooltip_text("Search Library (Ctrl+F)");
    search_btn->set_action_name("win.search");
    m_header.pack_end(search_btn);

    m_root.append(m_header);

    // ── Toolbar (below header, toggleable) ────────────────────────────────
    {
        const auto make_tool_btn = [](const char* icon, const char* tip,
                                       const char* action) {
            auto* b = Gtk::make_managed<Gtk::Button>();
            b->set_icon_name(icon);
            b->set_tooltip_text(tip);
            b->set_action_name(action);
            b->add_css_class("flat");
            return b;
        };

        auto* pause_btn = Gtk::make_managed<Gtk::ToggleButton>();
        pause_btn->set_icon_name("media-playback-pause-symbolic");
        pause_btn->set_tooltip_text("Pause / Resume (Space)");
        pause_btn->set_action_name("win.pause");
        pause_btn->add_css_class("flat");
        m_toolbar.pack_start(*pause_btn);
        m_toolbar.pack_start(*make_tool_btn(
            "view-refresh-symbolic",    "Reset Emulation",   "win.reset"));
        m_toolbar.pack_start(*make_tool_btn(
            "media-eject-symbolic",     "Power Off",         "win.power-off"));

        m_toolbar.pack_end(*make_tool_btn(
            "view-fullscreen-symbolic", "Toggle Fullscreen", "win.fullscreen"));
        m_toolbar.pack_end(*make_tool_btn(
            "camera-photo-symbolic",    "Screenshot",        "win.screenshot"));
    }
    m_toolbar.set_visible(m_config.iface.show_toolbar);
    m_root.append(m_toolbar);

    // ── Views ─────────────────────────────────────────────────────────────
    m_library_page  = std::make_unique<LibraryPage>(m_config, m_stats, &MainWindow::game_id_for);
    m_library_page->signal_game_action.connect(
        [this](std::string action, std::string path) { on_game_action(action, path); });
    m_render_widget = std::make_unique<RenderWidget>();

    m_library_page->set_library(&m_library);
    m_library_page->signal_launch_game.connect(
        [this](const std::string& path) { launch(path); });

    m_render_widget->signal_view_resized.connect([this] {
        if (m_config.graphics.internal_scale != 0 || !m_emu_running)
            return;
        // Wait for the size to settle (fullscreen transitions, dragging).
        m_auto_res_timer.disconnect();
        m_auto_res_timer = Glib::signal_timeout().connect([this] {
            if (m_emu_running && internal_resolution() != m_applied_resolution)
                apply_engine_option("internal_resolution");
            return false;
        }, 400);
    });
    m_render_widget->signal_started.connect([this](bool ok, std::string error) {
        if (ok)
            return;
        show_toast(error);
        close_game_now(false);
    });
    m_render_widget->signal_message.connect(
        [this](std::string text) { show_toast(text); });
    m_render_widget->signal_engine_shutdown.connect([this] {
        const bool online = m_netplay && !m_netplay_waiting;
        close_game_now(false);
        // Online the engine stops when the other side leaves or drops out.
        if (online)
            alert(*this, "Online Play Ended",
                  "The connection to the other player was lost, or they closed the game.",
                  {{"ok", "OK"}}, "ok", nullptr, [](const std::string&) {});
    });

    // Double-click the game image to toggle fullscreen.
    auto dbl = Gtk::GestureClick::create();
    dbl->set_button(GDK_BUTTON_PRIMARY);
    dbl->signal_pressed().connect([this](int n_press, double, double) {
        if (n_press == 2 && m_config.behavior.double_click_fullscreen)
            toggle_fullscreen();
    });
    m_game_overlay.add_controller(dbl);

    // Keep the action and window chrome right when the compositor changes
    // fullscreen state behind our back (e.g. a window-manager keybind).
    property_fullscreened().signal_changed().connect([this] {
        m_act_fullscreen->set_state(Glib::Variant<bool>::create(is_fullscreen()));
        apply_chrome_visibility();
    });
    m_views.property_visible_child().signal_changed().connect([this] {
        apply_chrome_visibility();
        update_cursor_and_inhibit();
        if (!m_emu_running || !m_config.behavior.pause_in_library)
            return;
        // Like DuckStation: looking at the game list pauses the game, and
        // coming back resumes it only if this is what paused it.
        const bool on_game = m_views.get_visible_child_name() == "game";
        if (!on_game && !m_render_widget->paused()) {
            m_paused_by_view = true;
            set_paused(true);
        } else if (on_game && m_paused_by_view) {
            m_paused_by_view = false;
            set_paused(false);
        }
    });

    // Window keys, then the keyboard fallback for controller port A while
    // the game view is showing.
    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType mods) {
            const bool alt  = (mods & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
            const bool ctrl = (mods & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{};
            if (alt && (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)) {
                toggle_fullscreen();
                return true;
            }
            // Window shortcuts.
            if (ctrl && (keyval == GDK_KEY_o || keyval == GDK_KEY_O)) { on_open_disc(); return true; }
            if (ctrl && (keyval == GDK_KEY_f || keyval == GDK_KEY_F)) {
                m_views.set_visible_child_name("library");
                m_act_search->change_state(Glib::Variant<bool>::create(true));
                return true;
            }
            if (keyval == GDK_KEY_F5 && m_views.get_visible_child_name() == "library") {
                rescan_library();
                return true;
            }
            // While the pause menu is up it owns the keyboard: Escape steps
            // back or closes it, everything else is ordinary GTK navigation.
            if (m_pause_menu && m_pause_menu->is_open()) {
                if (const HotkeyDef* hk = m_hotkeys.lookup(keyval);
                    hk && std::string(hk->id) == "pause-menu") {
                    if (!m_pause_menu->go_back())
                        close_pause_menu(true);
                    return true;
                }
                if (const HotkeyDef* hk = m_hotkeys.lookup(keyval);
                    hk && std::string(hk->id) == "toggle-fullscreen") {
                    toggle_fullscreen();
                    return true;
                }
                return false;
            }
            // Hotkeys. Fullscreen works everywhere; the rest need a game on screen.
            if (!ctrl && !alt) {
                if (const HotkeyDef* hk = m_hotkeys.lookup(keyval)) {
                    const bool game_view = m_emu_running &&
                                           m_views.get_visible_child_name() == "game";
                    if (game_view || std::string(hk->id) == "toggle-fullscreen") {
                        handle_hotkey(hk->id, true);
                        return true;
                    }
                }
            }
            if (!m_emu_running || m_views.get_visible_child_name() != "game")
                return false;
            if ((mods & (Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::ALT_MASK))
                    != Gdk::ModifierType{})
                return false;
            return m_render_widget->handle_key(keyval, true);
        }, false);
    keys->signal_key_released().connect(
        [this](guint keyval, guint, Gdk::ModifierType) {
            if (const HotkeyDef* hk = m_hotkeys.lookup(keyval); hk && hk->hold)
                handle_hotkey(hk->id, false);
            m_render_widget->handle_key(keyval, false);
        });
    add_controller(keys);

    // Game view = render surface + an OSD overlay (DuckStation-style corner FPS).
    m_osd_fps.add_css_class("osd-fps");
    m_osd_fps.set_halign(Gtk::Align::END);
    m_osd_fps.set_valign(Gtk::Align::START);
    m_osd_fps.set_margin_top(10);
    m_osd_fps.set_margin_end(10);
    m_osd_fps.set_visible(false);          // shown once emulation reports FPS
    m_game_overlay.set_child(*m_render_widget);
    m_osd = std::make_unique<OsdLayer>();
    m_game_overlay.add_overlay(*m_osd);
    m_pause_menu = std::make_unique<PauseMenu>();
    m_pause_menu->signal_action.connect(
        [this](std::string action) { on_pause_menu_action(action); });
    m_game_overlay.add_overlay(*m_pause_menu);

    m_views.add_titled_with_icon(
        m_library_page.get(), "library", "Library",
        "applications-games-symbolic");
    m_views.add_titled_with_icon(
        &m_game_overlay, "game", "Game",
        "applications-multimedia-symbolic");

    // Hide "Game" page until emulation starts
    if (auto p = m_views.get_page(&m_game_overlay))
        p->set_visible(false);

    m_views.set_vexpand(true);
    m_root.append(m_views);

    // ── Status bar ────────────────────────────────────────────────────────
    m_status_game_lbl.set_text("No game running");
    m_status_game_lbl.set_halign(Gtk::Align::START);
    m_status_game_lbl.add_css_class("dim-label");

    m_status_fps_lbl.set_text("");
    m_status_fps_lbl.set_hexpand(true);
    m_status_fps_lbl.set_halign(Gtk::Align::CENTER);

    m_status_scale_lbl.set_text("");
    m_status_scale_lbl.set_halign(Gtk::Align::END);
    m_status_scale_lbl.add_css_class("dim-label");

    m_status_bar.set_margin_start(12);
    m_status_bar.set_margin_end(12);
    m_status_bar.set_margin_top(4);
    m_status_bar.set_margin_bottom(4);
    m_status_bar.add_css_class("status-bar");
    m_status_bar.append(m_status_game_lbl);
    m_status_bar.append(m_status_fps_lbl);
    m_status_video_lbl.set_margin_end(24);
    m_status_bar.append(m_status_video_lbl);
    m_status_bar.append(m_status_scale_lbl);
    m_status_bar.set_visible(m_config.iface.show_status_bar);
    m_root.append(m_status_bar);

    // FPS timer: fires every 1000 ms to update the status bar
    m_fps_timer = Glib::signal_timeout().connect(
        [this]() -> bool {
            update_fps_display();
            if (m_emu_running && m_render_widget->running() && !m_render_widget->paused())
                ++m_session_seconds;
            return true;
        }, 1000);
}

// ── Action handlers ───────────────────────────────────────────────────────

void MainWindow::on_open_disc() {
    auto dlg = Gtk::FileDialog::create();
    dlg->set_title("Open Dreamcast Disc Image");

    auto filter = Gtk::FileFilter::create();
    filter->set_name("Dreamcast discs and Naomi/Atomiswave ROM sets");
    for (const char* pat : {"*.gdi", "*.cdi", "*.chd", "*.cue", "*.m3u",
                            "*.zip", "*.7z", "*.lst", "*.dat", "*.elf"})
        filter->add_pattern(pat);
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    filters->append(filter);
    dlg->set_filters(filters);

    dlg->open(*this, [this, dlg](const Glib::RefPtr<Gio::AsyncResult>& res) {
        try {
            auto file = dlg->open_finish(res);
            launch(file->get_path());
        } catch (...) {}
    });
}

void MainWindow::on_start_bios() {
    if (m_config.paths.bios.empty()) {
        show_toast("Configure the BIOS path in Settings → BIOS first");
        open_settings("bios");
        return;
    }
    const auto boot = [this] { start_emulation("", /*disc_less=*/true); };
    if (m_emu_running) request_close_game(boot);
    else               boot();
}

void MainWindow::on_pause_toggle(bool paused) {
    if (paused && m_netplay) {
        m_act_pause->set_state(Glib::Variant<bool>::create(false));
        netplay_blocks("Online games cannot be paused");
        return;
    }
    m_render_widget->set_paused(paused);
    m_status_game_lbl.set_text(paused ? m_current_game_title + " — Paused"
                                       : m_current_game_title);
    update_cursor_and_inhibit();
}

void MainWindow::on_reset() {
    if (netplay_blocks("Restart is off during online play"))
        return;
    if (m_emu_running) {
        m_render_widget->reset();
        show_toast("Emulation reset");
    }
}

void MainWindow::on_power_off() {
    if (m_emu_running)
        request_close_game();
}

void MainWindow::on_fullscreen_toggle(bool fs) {
    if (fs) fullscreen(); else unfullscreen();
    apply_chrome_visibility();
    update_cursor_and_inhibit();
}

void MainWindow::toggle_fullscreen() {
    m_act_fullscreen->change_state(Glib::Variant<bool>::create(!is_fullscreen()));
}

// Fullscreen on the game view shows nothing but the game.
void MainWindow::apply_chrome_visibility() {
    const bool bare = is_fullscreen() && m_views.get_visible_child_name() == "game";
    m_header.set_visible(!bare);
    m_toolbar.set_visible(!bare && m_config.iface.show_toolbar);
    m_status_bar.set_visible(!bare && m_config.iface.show_status_bar);
}

void MainWindow::on_resolution_changed(u32 scale) {
    m_config.graphics.internal_scale = std::min<u32>(scale, 8);
    m_config.save();
    apply_engine_option("internal_resolution");
    show_toast(std::string("Internal resolution: ") + (scale == 0 ? "automatic, " : "") +
               internal_resolution());
}

std::string MainWindow::widescreen_mode() const {
    const auto on = [this](const char* key) {
        const auto it = m_config.engine.find(key);
        return it != m_config.engine.end() && it->second == "enabled";
    };
    if (on("widescreen_cheats")) return "cheats";
    if (on("widescreen_hack"))   return "hack";
    return "off";
}

// The hack widens the 3D view of any game on the fly (2D overlays stay 4:3,
// and some games show pop-in at the edges). Cheats are per-game codes that
// make the game itself render for 16:9; they exist for a subset of games and
// take effect when the game starts.
void MainWindow::on_widescreen_changed(const std::string& mode) {
    const auto set = [this](const char* key, bool enabled) {
        if (enabled) m_config.engine[key] = "enabled";
        else         m_config.engine.erase(key);
        apply_engine_option(key);
    };
    const bool was_cheats = widescreen_mode() == "cheats";
    set("widescreen_hack",   mode == "hack");
    set("widescreen_cheats", mode == "cheats");
    m_config.save();
    apply_engine_option("internal_resolution");   // automatic size depends on the aspect
    if (mode == "cheats" || was_cheats)
        show_toast(m_emu_running ? "Widescreen cheats change when the game restarts"
                                 : "Widescreen cheats: used for games that have them");
    else
        show_toast(mode == "hack" ? "Widescreen hack on" : "Widescreen off");
}

void MainWindow::on_show_toolbar(bool v) {
    m_config.iface.show_toolbar = v;
    apply_chrome_visibility();
}

void MainWindow::on_show_status_bar(bool v) {
    m_config.iface.show_status_bar = v;
    apply_chrome_visibility();
}

void MainWindow::on_search_toggle(bool v) {
    if (m_library_page) m_library_page->set_search_visible(v);
}

void MainWindow::apply_post_chain() {
    if (m_render_widget)
        m_render_widget->set_post_chain(m_config.post_chain, m_config.post_enabled,
                                        m_config.post_native_input);
}

void MainWindow::on_open_data_dir() {
    const auto dir = Config::data_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    try {
        auto launcher = Gio::AppInfo::create_from_commandline(
            "xdg-open \"" + dir.string() + "\"", "", Gio::AppInfo::CreateFlags::NONE);
        launcher->launch(std::vector<Glib::RefPtr<Gio::File>>{},
                         Glib::RefPtr<Gio::AppLaunchContext>{});
    } catch (...) {
        show_toast("Could not open data directory: " + dir.string());
    }
}

void MainWindow::on_about() {
    auto* about = new Adw::AboutWindow();
    about->set_application_name("DreamStation");
    about->set_application_icon("io.github.johnnygentoo.DreamStation");
    about->set_version(DS_VERSION);
    about->set_developer_name("Johnnygentoo");
    about->set_comments(
        "A Sega Dreamcast, Naomi and Atomiswave emulator for Linux, built on the Flycast "
        "emulation engine.\n\n"
        "Dreamcast, Naomi and Sega are trademarks of Sega. Atomiswave is a trademark of "
        "Sammy. DreamStation is not affiliated with or endorsed by either.");
    about->set_license_type(Gtk::License::GPL_3_0);
    about->set_website("https://github.com/Johnnygentoo/dreamstation");
    about->set_issue_url("https://github.com/Johnnygentoo/dreamstation/issues");
    about->set_developers({"Johnnygentoo https://github.com/Johnnygentoo"});
    about->set_copyright("© 2026 Johnnygentoo");
    about->add_credit_section("Emulation engine", {
        "Flycast by flyinghead and contributors https://github.com/flyinghead/flycast",
        "Built on reicast and nullDC by their developers",
    });
    about->add_acknowledgement_section("Thanks to", {
        "ReShade by Patrick Mours (effect compiler) https://reshade.me",
        "libretro thumbnail project (cover art) https://github.com/libretro-thumbnails",
        "DuckStation by Stenzek (interface inspiration) https://www.duckstation.org",
    });
    about->add_legal_section("Flycast", "© flyinghead and the Flycast, reicast and nullDC contributors",
                             Gtk::License::GPL_2_0, "");
    about->add_legal_section("ReShade FX compiler", "© 2014 Patrick Mours",
                             Gtk::License::BSD_3, "");
    about->add_legal_section("GTK, gtkmm, libadwaita", "© The GTK and GNOME teams",
                             Gtk::License::LGPL_2_1, "");
    about->add_legal_section("SDL", "© Sam Lantinga and contributors", Gtk::License::CUSTOM,
                             "zlib licence");
    about->add_legal_section("spdlog, toml++", "© Gabi Melman; © Mark Gillard",
                             Gtk::License::MIT_X11, "");
    about->set_transient_for(*this);
    about->present();
}

std::vector<ds::engine::OptionInfo> MainWindow::engine_options() {
    // The option list comes from the engine itself, which needs initialising
    // once (it does not need a game or a BIOS for this).
    std::error_code ec;
    const auto system_dir = Config::data_dir() / "system";
    const auto save_dir   = Config::data_dir() / "saves";
    std::filesystem::create_directories(system_dir / "dc", ec);
    std::filesystem::create_directories(save_dir, ec);
    m_render_widget->prepare_engine(system_dir, save_dir);
    return ds::engine::Engine::instance().options();
}

// Effective value of an engine option for whatever is running:
// engine default < global setting < the running game's override.
void MainWindow::apply_engine_option(const std::string& key) {
    if (key == "internal_resolution" && (m_game_id.empty() ||
            !GameSettings::load(m_game_id).engine.count(key))) {
        m_applied_resolution = internal_resolution();
        m_render_widget->set_option(key, m_applied_resolution);
        if (m_emu_running)
            m_status_scale_lbl.set_text(m_applied_resolution);
        return;
    }
    std::string value;
    for (const auto& opt : ds::engine::Engine::instance().options())
        if (opt.key == key) { value = opt.default_value; break; }
    if (auto it = m_config.engine.find(key); it != m_config.engine.end())
        value = it->second;
    if (m_emu_running && !m_game_id.empty()) {
        const auto game = GameSettings::load(m_game_id);
        if (auto it = game.engine.find(key); it != game.engine.end())
            value = it->second;
    }
    if (!value.empty())
        m_render_widget->set_option(key, value);
}

void MainWindow::connect_settings(SettingsWindow& win) {
    win.set_transient_for(*this);
    win.set_hide_on_close(true);
    win.signal_engine_option.connect([this](std::string key) {
        apply_engine_option(key);
        if (key == "widescreen_hack" || key == "widescreen_cheats") {
            m_act_widescreen->set_state(Glib::Variant<Glib::ustring>::create(widescreen_mode()));
            apply_engine_option("internal_resolution");
        }
    });
    win.signal_post_chain.connect([this] {
        m_act_post->set_state(Glib::Variant<bool>::create(m_config.post_enabled));
        apply_post_chain();
    });
    win.signal_post_option.connect([this](std::size_t stage, std::string name, float value) {
        m_render_widget->set_post_option(stage, name, value);
    });
    win.signal_changed.connect([this] {
        // Propagate live changes to the frontend's own settings.
        m_act_toolbar->set_state(Glib::Variant<bool>::create(m_config.iface.show_toolbar));
        m_act_statusbar->set_state(Glib::Variant<bool>::create(m_config.iface.show_status_bar));
        m_act_auto_covers->set_state(Glib::Variant<bool>::create(m_config.library.auto_covers));
        m_act_box_covers->set_state(Glib::Variant<bool>::create(m_config.library.box_covers));
        apply_chrome_visibility();
        update_cursor_and_inhibit();
        rescan_library();
        m_act_resolution->set_state(Glib::Variant<Glib::ustring>::create(
            std::to_string(std::min<u32>(m_config.graphics.internal_scale, 8))));
        m_act_widescreen->set_state(Glib::Variant<Glib::ustring>::create(widescreen_mode()));
        apply_engine_option("internal_resolution");
        if (m_emu_running)
            m_status_scale_lbl.set_text(internal_resolution());
        apply_input_config();
        apply_audio_config();
        apply_display_config();
        apply_osd_config();
        m_config.save();
    });
}

void MainWindow::open_settings(const std::string& page) {
    if (!m_settings_win) {
        m_settings_win = std::make_unique<SettingsWindow>(m_config, engine_options());
        connect_settings(*m_settings_win);
        m_settings_win->signal_reset.connect([this] {
            // Rebuild the window from the cleared settings.
            Glib::signal_idle().connect_once([this] {
                for (const auto& opt : ds::engine::Engine::instance().options())
                    apply_engine_option(opt.key);
                m_settings_win.reset();
                open_settings("interface");
            });
        });
    }
    m_settings_win->show_page(page);
}

void MainWindow::open_game_properties(const std::filesystem::path& game) {
    if (game.empty()) {
        show_toast("Start a game to edit its properties");
        return;
    }
    SettingsWindow::GameContext ctx;
    ctx.id    = game_id_for(game);
    ctx.path  = game;
    ctx.title = game.stem().string();
    for (const auto& g : m_library.games()) {
        if (g.path == game) {
            ctx.title      = g.title;
            ctx.product_id = g.product_id;
            ctx.region     = g.region;
            ctx.disc_title = g.disc_title;
            break;
        }
    }
    m_game_settings_win = std::make_unique<SettingsWindow>(m_config, engine_options(), ctx);
    connect_settings(*m_game_settings_win);
    m_game_settings_win->signal_reset.connect([this, game] {
        Glib::signal_idle().connect_once([this, game] {
            for (const auto& opt : ds::engine::Engine::instance().options())
                apply_engine_option(opt.key);
            open_game_properties(game);
        });
    });
    m_game_settings_win->show_page("summary");
}

void MainWindow::apply_input_config() {
    PadInput::instance().set_bindings(m_config.bindings);
    PadInput::instance().set_rumble_enabled(m_config.behavior.rumble);
    m_hotkeys.rebuild(m_config.hotkeys, m_config.hotkey_pads);
    using Dev = ds::engine::Engine::PortDevice;
    for (unsigned port = 0; port < 4; ++port) {
        Dev dev = Dev::Controller;
        const auto it = m_config.bindings.find(std::string(1, static_cast<char>('A' + port)) + ".type");
        if (it != m_config.bindings.end()) {
            if (it->second == "arcade")         dev = Dev::ArcadeStick;
            else if (it->second == "twinstick") dev = Dev::TwinStick;
            else if (it->second == "racing")    dev = Dev::RacingWheel;
            else if (it->second == "none")      dev = Dev::None;
        }
        ds::engine::Engine::instance().set_port_device(port, dev);
    }
}

void MainWindow::open_controllers(const std::string& page) {
    if (!m_controllers_win) {
        m_controllers_win = std::make_unique<ControllersWindow>(m_config);
        m_controllers_win->set_transient_for(*this);
        m_controllers_win->set_hide_on_close(true);
        m_controllers_win->signal_changed.connect([this] { apply_input_config(); });
    }
    m_controllers_win->show_page(page);
}

// ── Emulation lifecycle ───────────────────────────────────────────────────

std::string MainWindow::internal_resolution() const {
    const u32 scale = std::min<u32>(m_config.graphics.internal_scale, 8);
    if (scale > 0)
        return std::to_string(640 * scale) + "x" + std::to_string(480 * scale);

    // Automatic: the smallest resolution the engine offers that has at least
    // as many lines as the image will occupy on screen, so a 4K display gets
    // 2160 lines (3840x2160 with the widescreen hack) and nothing is wasted.
    const double aspect = widescreen_mode() == "off" ? 4.0 / 3.0 : 16.0 / 9.0;
    const int view_w = m_render_widget ? m_render_widget->view_width() : 0;
    const int view_h = m_render_widget ? m_render_widget->view_height() : 0;
    const int lines  = view_w > 0 && view_h > 0
        ? std::min(view_h, static_cast<int>(std::lround(view_w / aspect))) : 1080;

    std::string best, largest;
    int best_h = 0, largest_h = 0;
    for (const auto& opt : ds::engine::Engine::instance().options()) {
        if (opt.key != "internal_resolution")
            continue;
        for (const auto& value : opt.values) {
            const auto x = value.find('x');
            if (x == std::string::npos || x == 0 || x + 1 >= value.size())
                continue;                                  // skip "2x"-style entries
            const int h = std::atoi(value.c_str() + x + 1);
            if (h > largest_h) { largest_h = h; largest = value; }
            if (h >= lines && (best_h == 0 || h < best_h)) { best_h = h; best = value; }
        }
    }
    ds::common::log()->debug("auto resolution: view {}x{} aspect {:.3f} -> {} lines -> {}",
                             view_w, view_h, aspect, lines, best.empty() ? largest : best);
    if (!best.empty())    return best;
    if (!largest.empty()) return largest;
    return "1440x1080";
}

// The engine looks for firmware in <system>/dc/. Build that directory inside
// our data dir from the files the user picked in Settings: the boot ROM and
// any arcade BIOS archives beside it are linked, the flash image is copied
// (the console writes to it, and the user's original should stay untouched).
bool MainWindow::prepare_system_dir(std::filesystem::path& system_dir, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    system_dir = Config::data_dir() / "system";
    const fs::path dc = system_dir / "dc";
    fs::create_directories(dc, ec);
    fs::create_directories(Config::data_dir() / "saves", ec);
    if (ec) {
        error = "Cannot create " + dc.string();
        return false;
    }
    if (!fs::exists(m_config.paths.bios)) {
        error = "Boot ROM not found: " + m_config.paths.bios.string();
        return false;
    }

    const auto link = [&](const fs::path& src, const std::string& name) {
        const fs::path dst = dc / name;
        std::error_code e;
        if (fs::is_symlink(dst, e) || fs::exists(dst, e))
            fs::remove(dst, e);
        fs::create_symlink(fs::absolute(src), dst, e);
    };
    link(m_config.paths.bios, "dc_boot.bin");
    for (const auto& entry : fs::directory_iterator(m_config.paths.bios.parent_path(), ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".zip")
            link(entry.path(), entry.path().filename().string());
    }
    // Arcade BIOS archives missing beside the boot ROM are taken from where
    // RetroArch keeps them, so Naomi 2 and System SP sets find theirs too.
    // The engine picks the archive each game needs when it loads.
    if (const char* home = std::getenv("HOME")) {
        const fs::path ra = fs::path(home) / ".config/retroarch/system";
        for (const char* name : {"naomi", "naomi2", "naomigd", "awbios", "segasp",
                                 "hod2bios", "f355bios", "f355dlx", "airlbios"}) {
            const std::string file = std::string(name) + ".zip";
            if (fs::exists(dc / file, ec))
                continue;
            for (const fs::path& dir : {ra / "dc", ra / "roms", ra / "Arcade", ra}) {
                if (fs::is_regular_file(dir / file, ec)) {
                    link(dir / file, file);
                    break;
                }
            }
        }
    }
    if (!m_config.paths.flash.empty() && fs::exists(m_config.paths.flash) &&
        !fs::exists(dc / "dc_flash.bin"))
        fs::copy_file(m_config.paths.flash, dc / "dc_flash.bin", ec);
    return true;
}

void MainWindow::start_emulation(const std::filesystem::path& disc, bool disc_less,
                                 const std::optional<ds::engine::Engine::Netplay>& netplay) {
    if (m_emu_running) stop_emulation();

    if (m_config.paths.bios.empty()) {
        show_toast("Boot ROM not configured — open Settings → BIOS");
        open_settings("bios");
        return;
    }

    RenderWidget::Session session;
    std::string error;
    if (netplay) {
        if (!prepare_netplay_dirs(session.system_dir, session.save_dir, error)) {
            show_toast(error);
            return;
        }
        // Both sides must emulate alike: every option at its default except
        // those that only change how the picture looks or how input is read
        // here. The game's own overrides are not used.
        static const std::set<std::string> kAffectsGame = {
            "broadcast", "cable_type", "emulate_framebuffer", "enable_rttb"};
        for (const auto& o : ds::engine::Engine::instance().options()) {
            const bool local = (o.category == "video" || o.category == "input" ||
                                o.category == "vmu" || o.key == "widescreen_hack") &&
                               !kAffectsGame.count(o.key);
            const auto it = m_config.engine.find(o.key);
            session.options[o.key] = local && it != m_config.engine.end() ? it->second
                                                                          : o.default_value;
        }
        session.options["internal_resolution"] = internal_resolution();
        session.netplay = netplay;
    } else {
        if (!prepare_system_dir(session.system_dir, error)) {
            show_toast(error);
            return;
        }
        session.save_dir = Config::data_dir() / "saves";
        for (const auto& [key, value] : m_config.engine)
            session.options[key] = value;
        session.options["internal_resolution"] = internal_resolution();
        // The game's own overrides win over the global settings.
        if (!disc_less && !disc.empty())
            for (const auto& [key, value] : GameSettings::load(game_id_for(disc)).engine)
                session.options[key] = value;
    }
    session.dns = m_config.network.dns;
    if (!disc_less && !disc.empty()) {
        session.game = disc;
        m_current_game_path  = disc;
        m_current_game_title = disc.stem().string();
        for (const auto& g : m_library.games())
            if (g.path == disc) { m_current_game_title = g.title; break; }
    } else {
        m_current_game_path.clear();
        m_current_game_title = "BIOS Menu";
    }
    m_game_id = game_id_for(m_current_game_path);
    m_session_seconds = 0;
    if (!m_current_game_path.empty()) {
        m_stats.touch(m_game_id);
        m_stats.save();
    }
    m_undo_state.clear();
    m_paused_by_view = m_paused_by_focus = m_paused_by_vmu = false;
    // A mini-game played from a card the new game will use must end first.
    if (m_vmu_window)
        m_vmu_window->stop_all();
    m_vmu_serial.fill(0);
    m_vmu_watch.fill({});
    m_vmu_timer.disconnect();
    m_vmu_timer = Glib::signal_timeout().connect(sigc::mem_fun(*this, &MainWindow::poll_vmus), 33);

    m_emu_running = true;
    m_act_pause->set_state(Glib::Variant<bool>::create(false));
    m_status_game_lbl.set_text(m_current_game_title);
    m_applied_resolution = session.options["internal_resolution"];
    m_status_scale_lbl.set_text(m_applied_resolution);
    set_game_visible(true);
    update_sensitivity();

    // The game page must be showing before the session starts: the engine is
    // loaded from the render callback, which needs the widget realised.
    m_views.set_visible_child_name("game");
    m_view_title.set_subtitle(m_current_game_title);
    m_netplay = netplay.has_value();
    m_netplay_waiting = m_netplay;
    m_netplay_host = netplay && netplay->host;
    apply_display_config();              // online: full speed
    m_render_widget->start(std::move(session));
    m_render_widget->grab_focus();
    if (m_netplay) {
        // The game starts once both sides are connected and in step.
        m_netplay_timer.disconnect();
        m_netplay_timer = Glib::signal_timeout().connect(sigc::mem_fun(*this, &MainWindow::poll_netplay), 100);
        const std::string port = std::to_string(ds::engine::Engine::kNetplayPort);
        alert(*this, netplay->host ? "Waiting for Your Friend" : "Connecting",
              netplay->host ? "Hosting " + m_current_game_title + " on UDP port " + port +
                                  ". The game starts when your friend joins."
                            : "Joining " + netplay->peer + " for " + m_current_game_title +
                                  ". The game starts when you are both connected.",
              {{"cancel", "Cancel"}}, "cancel", nullptr, [this](const std::string&) {
                  if (!m_netplay_waiting)
                      return;           // closed because the game started
                  m_netplay_waiting = false;
                  ds::engine::Engine::instance().cancel_netplay();
                  close_game_now(false);
              });
    }
    rebuild_state_menus();
    rebuild_disc_menu();
    update_cursor_and_inhibit();
    if (m_config.behavior.start_fullscreen && !is_fullscreen())
        toggle_fullscreen();
}

void MainWindow::stop_emulation() {
    flush_play_time();
    m_emu_running = false;
    m_paused_by_vmu = false;
    m_netplay_timer.disconnect();
    if (m_netplay_waiting) {
        m_netplay_waiting = false;
        if (g_open_alert)
            adw_dialog_force_close(g_open_alert);
    }
    const bool was_online = m_netplay;
    m_netplay = false;
    m_vmu_timer.disconnect();
    if (m_osd)
        m_osd->set_vmu_screen(nullptr);
    if (m_vmu_window) {
        for (int port = 0; port < 4; ++port)
            m_vmu_window->show_screen(port, nullptr);
        Glib::signal_idle().connect_once([this] { if (m_vmu_window) m_vmu_window->refresh(); });
    }
    if (m_rescan_after_game)
        Glib::signal_idle().connect_once([this] { rescan_library(); });
    if (m_render_widget) m_render_widget->stop();
    if (was_online)
        apply_display_config();

    m_current_game_title.clear();
    m_current_game_path.clear();
    m_game_id.clear();
    m_undo_state.clear();
    m_view_title.set_subtitle("");
    update_sensitivity();
    if (m_load_menu) {
        rebuild_state_menus();
        rebuild_disc_menu();
        update_cursor_and_inhibit();
    }
}

// ── UI helpers ────────────────────────────────────────────────────────────

// Notifications: over the game they are on-screen-display messages, as in
// DuckStation; elsewhere they are ordinary toasts.
void MainWindow::show_toast(const Glib::ustring& msg) {
    const bool on_game = m_emu_running && m_views.get_visible_child_name() == "game" &&
                         !(m_pause_menu && m_pause_menu->is_open());
    if (on_game && m_osd) {
        if (m_config.osd.messages)
            m_osd->message(msg);
        return;
    }
    AdwToast* raw = adw_toast_new(msg.c_str());
    adw_toast_set_timeout(raw, 3);
    adw_toast_overlay_add_toast(m_toasts.gobj(), raw);
}

void MainWindow::update_fps_display() {
    if (!m_emu_running || !m_render_widget->running()) {
        m_status_fps_lbl.set_text("");
        m_status_video_lbl.set_text("");
        if (m_osd) m_osd->set_performance({});
        m_last_presented = m_last_emulated = 0;
        return;
    }
    // "Game" = new images the game drew; "Video" = emulated frames, i.e. speed.
    const u64 presented = m_render_widget->frames_presented();
    const u64 emulated  = m_render_widget->frames_emulated();
    const u64 game_fps  = presented - m_last_presented;
    const u64 video_fps = emulated - m_last_emulated;
    m_last_presented = presented;
    m_last_emulated  = emulated;
    const int speed = static_cast<int>(std::lround(100.0 * static_cast<double>(video_fps) / 59.94));
    m_status_fps_lbl.set_text("Game: " + std::to_string(game_fps) + " FPS");
    m_status_video_lbl.set_text("Video: " + std::to_string(video_fps) + " FPS (" +
                                std::to_string(speed) + "%)");

    // Performance read-out, as configured in Settings → On-Screen Display.
    double avg_ms = 0.0, worst_ms = 0.0;
    m_render_widget->take_frame_times(avg_ms, worst_ms);
    std::vector<std::string> lines;
    const auto& o = m_config.osd;
    if (m_render_widget->paused())            lines.push_back("PAUSED");
    else if (m_render_widget->fast_forward()) lines.push_back("FAST FORWARD");
    else if (m_render_widget->speed() != 100) lines.push_back(std::to_string(m_render_widget->speed()) + "% SPEED");
    if (o.show_fps)        lines.push_back("G: " + std::to_string(game_fps) + " FPS");
    if (o.show_speed)      lines.push_back("V: " + std::to_string(video_fps) + " FPS  " +
                                           std::to_string(speed) + "%");
    if (o.show_resolution) lines.push_back(std::to_string(m_render_widget->frame_width()) + "x" +
                                           std::to_string(m_render_widget->frame_height()));
    if (o.show_frame_times) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f ms avg  %.1f ms max", avg_ms, worst_ms);
        lines.push_back(buf);
    }
    m_osd->set_performance(lines);
}

void MainWindow::set_game_visible(bool v) {
    if (auto p = m_views.get_page(&m_game_overlay))
        p->set_visible(v);
}

void MainWindow::update_sensitivity() {
    const bool running = m_emu_running;
    // Pause/Reset/PowerOff only make sense when emulation is active.
    if (m_act_pause) m_act_pause->set_enabled(running);
    if (!m_action_group)
        return;
    for (const char* name : {"reset", "power-off", "power-off-nosave", "screenshot",
                             "save-slot", "undo-load-state", "change-disc"}) {
        if (auto a = std::dynamic_pointer_cast<Gio::SimpleAction>(
                m_action_group->lookup_action(name)))
            a->set_enabled(running);
    }
    // (open-file, start-bios always enabled)
}

void MainWindow::rescan_library() {
    m_rescan_timer.disconnect();
    m_rescan_after_game = false;
    forget_cover_index();       // other programs may have fetched covers meanwhile
    m_library.scan(m_config.paths.game_dirs, m_config.paths.excluded);
    if (m_config.library.auto_covers)
        fetch_missing_covers();
    watch_game_dirs();
}

// Watches the search directories and the folders in them (games live in
// folders of their own, a few levels deep at most), so that added, removed
// or renamed games show up without a manual rescan.
void MainWindow::watch_game_dirs() {
    m_dir_monitors.clear();
    if (!m_config.library.watch_dirs)
        return;
    constexpr int kMaxDepth = 3, kMaxFolders = 4096;
    const auto watch = [this](const std::filesystem::path& dir) {
        try {
            auto monitor = Gio::File::create_for_path(dir.string())
                               ->monitor_directory(Gio::FileMonitorFlags::WATCH_MOVES);
            monitor->signal_changed().connect(
                [this](const Glib::RefPtr<Gio::File>&, const Glib::RefPtr<Gio::File>&,
                       Gio::FileMonitor::Event event) {
                    if (event != Gio::FileMonitor::Event::ATTRIBUTE_CHANGED)
                        on_game_dir_changed();
                });
            m_dir_monitors.push_back(std::move(monitor));
        } catch (const Glib::Error&) {
            // Not watchable (gone, or no permission): a manual rescan still works.
        }
    };
    std::set<std::filesystem::path> visited;   // real folders, as the scan follows links
    for (const auto& root : m_config.paths.game_dirs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec) ||
            !visited.insert(std::filesystem::weakly_canonical(root, ec)).second)
            continue;
        watch(root);
        for (auto it = std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::follow_directory_symlink |
                       std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it.depth() >= kMaxDepth - 1)
                it.disable_recursion_pending();
            std::error_code dec;
            if (!it->is_directory(dec))
                continue;
            if (!visited.insert(std::filesystem::weakly_canonical(it->path(), dec)).second) {
                it.disable_recursion_pending();
                continue;
            }
            if (m_dir_monitors.size() >= kMaxFolders)
                return;
            watch(it->path());
        }
    }
}

// Copying a game sends a stream of changes: rescan once they have stopped
// for a few seconds, so a half-copied game is not listed.
void MainWindow::on_game_dir_changed() {
    m_rescan_timer.disconnect();
    m_rescan_timer = Glib::signal_timeout().connect([this] {
        if (m_emu_running) {
            m_rescan_after_game = true;      // no scan hitch during play
        } else {
            ds::common::log()->info("Game folders changed; rescanning");
            rescan_library();
        }
        return false;
    }, 3000);
}

void MainWindow::ensure_cover_downloader() {
    if (m_cover_downloader)
        return;
    m_cover_downloader = std::make_unique<CoverDownloader>(m_config, m_library);
    m_cover_downloader->set_transient_for(*this);
    m_cover_downloader->set_hide_on_close(true);
    // Covers arrive one after another; show them in batches rather than
    // rebuilding the game list for each.
    m_cover_downloader->signal_cover_saved.connect([this] {
        if (m_cover_refresh.connected())
            return;
        m_cover_refresh = Glib::signal_timeout().connect([this] {
            m_library_page->refresh();
            return false;
        }, 2000);
    });
}

// ── Online play ───────────────────────────────────────────────────────────

void MainWindow::open_netplay_dialog(const std::filesystem::path& game) {
    m_netplay_dialog = std::make_unique<NetplayDialog>(m_config, m_library, game);
    m_netplay_dialog->set_transient_for(*this);
    m_netplay_dialog->signal_start.connect(
        [this](std::filesystem::path path, ds::engine::Engine::Netplay netplay) {
            const auto start = [this, path, netplay] {
                start_emulation(path, false, netplay);
            };
            if (m_emu_running) request_close_game(start);
            else               start();
        });
    m_netplay_dialog->present();
}

// An online game starts from the same state on both sides: the BIOS files,
// and no saved settings, VMUs or arcade EEPROMs, so the engine creates its
// defaults. (Saves made online are thrown away with the folder next time.)
bool MainWindow::prepare_netplay_dirs(std::filesystem::path& system_dir,
                                      std::filesystem::path& save_dir, std::string& error) {
    namespace fs = std::filesystem;
    fs::path base;
    if (!prepare_system_dir(base, error))
        return false;
    const fs::path root = Config::data_dir() / "netplay";
    std::error_code ec;
    fs::remove_all(root, ec);
    system_dir = root / "system";
    save_dir   = root / "saves";
    fs::create_directories(system_dir / "dc", ec);
    fs::create_directories(save_dir, ec);
    if (ec) {
        error = "Cannot create " + root.string();
        return false;
    }
    for (const auto& e : fs::directory_iterator(base / "dc", ec)) {
        const std::string name = e.path().filename().string();
        if (name != "dc_boot.bin" && e.path().extension() != ".zip")
            continue;
        std::error_code le;
        fs::create_symlink(fs::canonical(e.path(), le), system_dir / "dc" / name, le);
    }
    // The flash dump as it came, not as this computer's BIOS has changed it.
    if (!m_config.paths.flash.empty() && fs::exists(m_config.paths.flash, ec))
        fs::copy_file(m_config.paths.flash, system_dir / "dc" / "dc_flash.bin", ec);
    return true;
}

bool MainWindow::poll_netplay() {
    if (!m_emu_running || !m_netplay)
        return false;
    std::string message;
    const auto state = ds::engine::Engine::instance().netplay_state(&message);
    using State = ds::engine::Engine::NetplayState;
    if (state == State::Connecting || state == State::Off)   // Off: not loaded yet
        return true;
    const bool host = m_netplay_host;
    if (m_netplay_waiting) {
        m_netplay_waiting = false;
        if (g_open_alert)
            adw_dialog_force_close(g_open_alert);
    }
    if (state == State::Running) {
        ds::common::log()->info("Online play: connected");
        show_toast(host ? "Connected — you are player 1" : "Connected — you are player 2");
        return false;
    }
    ds::common::log()->warn("Online play failed: {}", message);
    close_game_now(false);
    alert(*this, "Online Play Stopped",
          message.empty() ? "The connection could not be made." : message,
          {{"ok", "OK"}}, "ok", nullptr, [](const std::string&) {});
    return false;
}

bool MainWindow::netplay_blocks(const char* what) {
    if (!m_netplay)
        return false;
    if (what)
        show_toast(what);
    return true;
}

// ── VMUs during a game ────────────────────────────────────────────────────

void MainWindow::ensure_vmu_window() {
    if (m_vmu_window)
        return;
    VmuWindow::Hooks hooks;
    hooks.game_running = [this] { return m_emu_running; };
    hooks.card_path    = [this](int port) { return vmu_card_path(port); };
    hooks.take_out     = [this](int port, bool out) { return take_vmu_out(port, out); };
    m_vmu_window = std::make_unique<VmuWindow>(std::move(hooks),
                                               m_config.audio.muted ? 0 : m_config.audio.volume);
    m_vmu_window->set_hide_on_close(true);
    m_vmu_window->signal_close_request().connect([this] {
        m_vmu_window->stop_all();
        m_act_vmu_window->set_state(Glib::Variant<bool>::create(false));
        m_config.iface.vmu_window = false;
        m_config.save();
        return false;
    }, false);
}

void MainWindow::set_vmu_window_visible(bool visible) {
    m_act_vmu_window->set_state(Glib::Variant<bool>::create(visible));
    if (m_config.iface.vmu_window != visible) {
        m_config.iface.vmu_window = visible;
        m_config.save();
    }
    if (visible) {
        ensure_vmu_window();
        m_vmu_serial.fill(0);            // send it the current screens
        m_vmu_window->refresh();
        m_vmu_window->present();
    } else if (m_vmu_window) {
        m_vmu_window->stop_all();
        m_vmu_window->set_visible(false);
    }
}

// The card the engine uses for that controller's VMU: its own answer while
// a game is loaded, else where it keeps cards.
std::filesystem::path MainWindow::vmu_card_path(int port) const {
    auto& engine = ds::engine::Engine::instance();
    if (m_emu_running && engine.game_loaded()) {
        auto path = engine.vmu_path(static_cast<unsigned>(port), 0);
        if (!path.empty())
            return path;
    }
    return Config::data_dir() / "system" / "dc" /
           ("vmu_save_" + std::string(1, static_cast<char>('A' + port)) + "1.bin");
}

// Out: the game loses the VMU (and is paused, as the player has put the
// controller down for it); back: it gets it again, reading the card afresh.
bool MainWindow::take_vmu_out(int port, bool out) {
    if (!m_emu_running)
        return true;
    if (out && netplay_blocks("VMUs stay in during online play"))
        return false;
    ds::engine::Engine::instance().set_vmu_unplugged(static_cast<unsigned>(port), 0, out);
    if (out) {
        if (!m_render_widget->paused()) {
            set_paused(true);
            m_paused_by_vmu = true;
        }
        show_toast("VMU " + std::string(1, static_cast<char>('A' + port)) + "1 taken out");
    } else {
        // What the mini-game left on the card is not news to announce.
        auto& w = m_vmu_watch[static_cast<std::size_t>(port)];
        w.fingerprint = vmu_game_fingerprint(port);
        std::error_code ec;
        w.time = std::filesystem::last_write_time(vmu_card_path(port), ec);
        w.quiet = -1;
        if (m_paused_by_vmu) {
            m_paused_by_vmu = false;
            set_paused(false);
        }
        show_toast("VMU " + std::string(1, static_cast<char>('A' + port)) + "1 plugged back in");
    }
    return true;
}

// The mini-game on that port's card, as name and contents, empty if none.
std::string MainWindow::vmu_game_fingerprint(int port, std::string* title) const {
    const auto card = VmuImage::load(vmu_card_path(port));
    if (!card || !card->valid())
        return {};
    for (const auto& f : card->files())
        if (f.game) {
            if (title)
                *title = f.long_description.empty() ? f.name : f.long_description;
            const auto data = card->read(f);
            return f.name + ":" + std::to_string(std::hash<std::string_view>{}(
                std::string_view(reinterpret_cast<const char*>(data.data()), data.size())));
        }
    return {};
}

// About 30 times a second during a game: new VMU screens go to the overlay
// and the VMU window; once a second the cards are checked for a mini-game
// the game has just put on one.
bool MainWindow::poll_vmus() {
    auto& engine = ds::engine::Engine::instance();
    if (!m_emu_running || !engine.game_loaded())
        return true;
    const bool overlay = m_config.osd.vmu_screen && m_osd;
    const bool window  = m_vmu_window && m_vmu_window->get_visible();
    if (overlay || window) {
        ds::engine::Engine::VmuDots dots;
        for (unsigned port = 0; port < 4; ++port) {
            std::uint64_t serial = 0;
            const bool has = engine.vmu_screen(port, 0, dots, serial);
            if (has && serial == m_vmu_serial[port])
                continue;
            m_vmu_serial[port] = has ? serial : 0;
            if (overlay && port == 0)
                m_osd->set_vmu_screen(has ? dots.data() : nullptr);
            if (window)
                m_vmu_window->show_screen(static_cast<int>(port), has ? &dots : nullptr);
        }
    }

    if (++m_vmu_ticks % 30 != 0)
        return true;
    for (int port = 0; port < 4; ++port) {
        auto& w = m_vmu_watch[static_cast<std::size_t>(port)];
        if (!engine.vmu_present(static_cast<unsigned>(port), 0))
            continue;
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(vmu_card_path(port), ec);
        if (ec)
            continue;
        if (!w.known) {
            w.known = true;
            w.time = time;
            w.fingerprint = vmu_game_fingerprint(port);
            continue;
        }
        if (time != w.time) {            // being written: wait for it to settle
            w.time = time;
            w.quiet = 0;
            continue;
        }
        if (w.quiet < 0 || ++w.quiet < 2)
            continue;
        w.quiet = -1;
        std::string title;
        const std::string fingerprint = vmu_game_fingerprint(port, &title);
        if (fingerprint == w.fingerprint)
            continue;
        w.fingerprint = fingerprint;
        if (fingerprint.empty())
            continue;
        // A game handed a mini-game to the VMU: offer it in the VMU window.
        ds::common::log()->info("VMU {}1: the game put {} on it", char('A' + port), title);
        set_vmu_window_visible(true);
        m_vmu_window->announce(port, title);
        show_toast(title + " is on VMU " + std::string(1, static_cast<char>('A' + port)) +
                   "1 — play it in the VMU window");
    }
    return true;
}

// Runs the cover downloader unseen for the games that have no cover; it can
// be watched or stopped from Tools > Cover Downloader.
void MainWindow::fetch_missing_covers() {
    ensure_cover_downloader();
    if (!m_cover_downloader->running())
        m_cover_downloader->start();
}

void MainWindow::play_vmu_game(const std::filesystem::path& source) {
    // A card the running game has plugged in must not be written behind its back.
    const std::string dir = (Config::data_dir() / "system" / "dc").lexically_normal().string();
    const std::string saves = (Config::data_dir() / "saves").lexically_normal().string();
    const std::string path = source.lexically_normal().string();
    if (m_emu_running && (path.rfind(dir, 0) == 0 || path.rfind(saves, 0) == 0)) {
        show_toast("Close the game first — it has this VMU plugged in");
        return;
    }
    if (m_vmu_player)
        m_vmu_player->close();     // saves its progress
    auto player = std::make_unique<VmuPlayer>(source, m_config.audio.muted ? 0 : m_config.audio.volume);
    if (!player->ok()) {
        show_toast(player->error());
        return;
    }
    m_vmu_player = std::move(player);
    m_vmu_player->set_transient_for(*this);
    m_vmu_player->set_hide_on_close(true);
    // Once closed (progress saved by then), let go of it and its audio device.
    VmuPlayer* shown = m_vmu_player.get();
    m_vmu_player->signal_hide().connect([this, shown] {
        Glib::signal_idle().connect_once([this, shown] {
            if (m_vmu_player.get() == shown)
                m_vmu_player.reset();
        });
    });
    m_vmu_player->present();
}

} // namespace ds::frontend
