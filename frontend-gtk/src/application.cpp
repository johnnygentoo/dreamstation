// SPDX-License-Identifier: GPL-3.0-or-later
#include "application.hpp"
#include <cstdlib>
#include "ui/main_window.hpp"
#include <dreamstation/common/log.hpp>
#include <dreamstation/engine/engine.hpp>
#include <glib.h>

namespace ds::frontend {

namespace {
// GLib log handler: silently drop the Adwaita warning that fires when the
// user's GTK theme (e.g. JohnnyCyan) sets gtk-application-prefer-dark-theme
// in ~/.config/gtk-4.0/settings.ini.  That GtkSettings key is unsupported
// by libadwaita, but the correct API (AdwStyleManager) is used here, so the
// warning is a false alarm from the theme, not the app.
void suppress_adwaita_theme_warning(const gchar* /*domain*/,
                                     GLogLevelFlags /*level*/,
                                     const gchar*   msg,
                                     gpointer       /*user_data*/)
{
    if (g_str_has_prefix(msg,
            "Using GtkSettings:gtk-application-prefer-dark-theme"))
        return;  // swallow
    g_log_default_handler("Adwaita", G_LOG_LEVEL_WARNING, msg, nullptr);
}
} // namespace

// Scripted test runs get their own process even while the user has
// DreamStation open; otherwise they would just hand over to that instance.
static Gio::Application::Flags app_flags() {
    auto flags = Gio::Application::Flags::HANDLES_OPEN;
    if (std::getenv("DS_TEST_SCRIPT") || std::getenv("DS_SELFTEST_SHOT"))
        flags |= Gio::Application::Flags::NON_UNIQUE;
    return flags;
}

Application::Application()
    : Adw::Application("io.github.johnnygentoo.DreamStation", app_flags()) {}

Glib::RefPtr<Application> Application::create() {
    return Glib::make_refptr_for_instance<Application>(new Application());
}

void Application::on_startup() {
    Adw::Application::on_startup();
    g_log_set_handler("Adwaita", G_LOG_LEVEL_WARNING,
                       suppress_adwaita_theme_warning, nullptr);
    Adw::StyleManager::get_default()->set_color_scheme(Adw::ColorScheme::PREFER_DARK);

    // Load app-level stylesheet.
    auto css = Gtk::CssProvider::create();
    css->load_from_resource("/io/github/johnnygentoo/DreamStation/style.css");
    Gtk::StyleContext::add_provider_for_display(
        Gdk::Display::get_default(), css,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

void Application::on_activate() {
    if (!m_window) {
        m_config = Config::load();
        m_window = std::make_unique<MainWindow>(m_config);
        add_window(*m_window);
    }
    m_window->present();
}

// Tear down in order: the window unloads the running game while its GL
// context still exists, then the engine stops its worker threads. Leaving
// those running makes process exit block in static destruction.
void Application::on_shutdown() {
    m_window.reset();
    ds::engine::Engine::instance().shutdown();
    Adw::Application::on_shutdown();
}

// `dreamstation game.chd` — open the window and boot the file.
void Application::on_open(const type_vec_files& files, const Glib::ustring&) {
    on_activate();
    if (!files.empty() && m_window)
        m_window->launch(files.front()->get_path());
}

} // namespace ds::frontend
