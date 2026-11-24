// SPDX-License-Identifier: GPL-3.0-or-later
//
// MainWindow: save-state slots, resume states, screenshots, hotkey actions
// and the close/launch prompts. Behaviour follows DuckStation's.
#include "main_window.hpp"
#include "../image_io.hpp"
#include "render_widget.hpp"
#include "settings_window.hpp"
#include "library_page.hpp"
#include "cover_downloader.hpp"
#include "controllers_window.hpp"
#include "pause_menu.hpp"
#include "vmu_manager.hpp"
#include "vmu_player.hpp"
#include "vmu_window.hpp"
#include "netplay_dialog.hpp"
#include "alert.hpp"
#include "osd_layer.hpp"
#include "../host/pad_input.hpp"
#include "../config.hpp"
#include <dreamstation/common/log.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <utility>

namespace ds::frontend {

namespace fs = std::filesystem;

namespace {

std::string timestamp_of(const fs::path& file) {
    std::error_code ec;
    const auto ftime = fs::last_write_time(file, ec);
    if (ec)
        return {};
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t t = std::chrono::system_clock::to_time_t(sys);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&t));
    return buf;
}

std::string now_for_filename() {
    const std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", std::localtime(&t));
    return buf;
}

fs::path sidecar(fs::path file, const char* ext) {
    file.replace_extension(ext);
    return file;
}

} // namespace

// ── Naming ────────────────────────────────────────────────────────────────

std::string MainWindow::game_id_for(const fs::path& game) {
    if (game.empty())
        return "bios";
    std::string name = game.stem().string();
    if (GameLibrary::is_generic_name(name)) {
        // disc.gdi says nothing; identify by its folder(s).
        name = game.parent_path().filename().string();
        if (name.size() <= 7 && game.parent_path().has_parent_path())
            name = game.parent_path().parent_path().filename().string() + "_" + name;
    }
    for (char& c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '.')
            c = '_';
    return name;
}

fs::path MainWindow::slot_path(bool global, int slot) const {
    const std::string base = global ? std::string("global") : m_game_id;
    return Config::savestates_dir() / (base + "_" + std::to_string(slot) + ".sav");
}

fs::path MainWindow::resume_path(const std::string& game_id) const {
    return Config::savestates_dir() / (game_id + "_resume.sav");
}

// ── State files ───────────────────────────────────────────────────────────

// A state is three files: the engine state, a screenshot (.png) for the
// menus and prompts, and the path of the game it belongs to (.game) so a
// state can boot its own game.
bool MainWindow::save_state_file(const fs::path& file) {
    if (netplay_blocks("Save states are off during online play"))
        return false;
    if (!m_emu_running || !m_render_widget->save_state(file))
        return false;
    if (auto shot = m_render_widget->capture_frame()) {
        const int w = std::min(640, shot->get_width());
        const int h = std::max(1, shot->get_height() * w / std::max(1, shot->get_width()));
        try {
            if (!image::save_png(shot->scale_simple(w, h, Gdk::InterpType::BILINEAR),
                                 sidecar(file, ".png")))
                ds::common::log()->warn("state screenshot: cannot write {}", sidecar(file, ".png").string());
        } catch (const Glib::Error& e) {
            ds::common::log()->warn("state screenshot: {}", e.what());
        }
    }
    std::ofstream(sidecar(file, ".game")) << m_current_game_path.string();
    return true;
}

bool MainWindow::load_state_file(const fs::path& file) {
    if (!m_emu_running || netplay_blocks("Save states are off during online play"))
        return false;
    std::vector<char> before;
    const bool have_undo = m_render_widget->save_state_to(before);
    if (!m_render_widget->load_state(file))
        return false;
    if (have_undo)
        m_undo_state = std::move(before);
    return true;
}

void MainWindow::save_slot(bool global, int slot) {
    if (!m_emu_running)
        return;
    const std::string name = std::string(global ? "Global" : "Game") + " Slot " + std::to_string(slot);
    show_toast(save_state_file(slot_path(global, slot)) ? "Saved to " + name
                                                        : "Could not save to " + name);
    rebuild_state_menus();
}

void MainWindow::load_slot(bool global, int slot) {
    const std::string name = std::string(global ? "Global" : "Game") + " Slot " + std::to_string(slot);
    const fs::path file = global ? Config::savestates_dir() / ("global_" + std::to_string(slot) + ".sav")
                                 : slot_path(false, slot);
    if (!global && !m_emu_running)
        return;
    if (!fs::exists(file)) {
        show_toast(name + " is empty");
        return;
    }
    // A global state may belong to another game: boot that game first.
    if (global) {
        std::string game;
        std::getline(std::ifstream(sidecar(file, ".game")), game);
        if (!m_emu_running || fs::path(game) != m_current_game_path) {
            if (game.empty() || !fs::exists(game)) {
                show_toast(name + ": its game could not be found");
                return;
            }
            const auto boot = [this, game, file] {
                start_emulation(game);
                m_render_widget->load_state_after_boot(file);
            };
            if (m_emu_running)
                request_close_game(boot);
            else
                boot();
            return;
        }
    }
    show_toast(load_state_file(file) ? "Loaded " + name : "Could not load " + name);
}

void MainWindow::on_state_action(const std::string& target, bool save) {
    if (target == "file") {
        auto dlg = Gtk::FileDialog::create();
        dlg->set_title(save ? "Save State To File" : "Load State From File");
        dlg->set_initial_folder(Gio::File::create_for_path(Config::savestates_dir().string()));
        auto filter = Gtk::FileFilter::create();
        filter->set_name("Save states");
        filter->add_pattern("*.sav");
        auto filters = Gio::ListStore<Gtk::FileFilter>::create();
        filters->append(filter);
        dlg->set_filters(filters);
        if (save) {
            if (!m_emu_running) return;
            dlg->set_initial_name(m_game_id + ".sav");
            dlg->save(*this, [this, dlg](const Glib::RefPtr<Gio::AsyncResult>& res) {
                try {
                    const auto file = dlg->save_finish(res);
                    show_toast(save_state_file(file->get_path()) ? "State saved" : "Could not save state");
                } catch (...) {}
            });
        } else {
            dlg->open(*this, [this, dlg](const Glib::RefPtr<Gio::AsyncResult>& res) {
                try {
                    const fs::path file = dlg->open_finish(res)->get_path();
                    if (!m_emu_running) {
                        std::string game;
                        std::getline(std::ifstream(sidecar(file, ".game")), game);
                        if (game.empty() || !fs::exists(game)) {
                            show_toast("Start the game first, then load this state");
                            return;
                        }
                        start_emulation(game);
                        m_render_widget->load_state_after_boot(file);
                        return;
                    }
                    show_toast(load_state_file(file) ? "State loaded" : "Could not load state");
                } catch (...) {}
            });
        }
        return;
    }
    const auto colon = target.find(':');
    if (colon == std::string::npos)
        return;
    const bool global = target.compare(0, colon, "global") == 0;
    const int  slot   = std::atoi(target.c_str() + colon + 1);
    if (slot < 1 || slot > kSlots)
        return;
    if (save) save_slot(global, slot);
    else      load_slot(global, slot);
}

void MainWindow::undo_load_state() {
    if (netplay_blocks("Save states are off during online play"))
        return;
    if (!m_emu_running || m_undo_state.empty()) {
        show_toast("Nothing to undo");
        return;
    }
    const bool ok = m_render_widget->load_state_from(m_undo_state);
    m_undo_state.clear();
    show_toast(ok ? "Load state undone" : "Could not undo the load");
}

void MainWindow::apply_osd_config() {
    if (!m_osd)
        return;
    m_osd->set_message_corner(m_config.osd.position);
    m_osd->set_scale(m_config.osd.scale);
    if (!m_config.osd.messages)
        m_osd->clear_messages();
}

// The slot selector, DuckStation-style: what is in the selected slot and
// which keys act on it.
void MainWindow::show_slot_osd() {
    OsdLayer::SlotInfo info;
    info.title = std::string(m_selected_global ? "Global" : "Game") + " Slot " +
                 std::to_string(m_selected_slot);
    const fs::path file = slot_path(m_selected_global, m_selected_slot);
    if (fs::exists(file)) {
        info.detail     = "Saved at " + timestamp_of(file);
        info.screenshot = sidecar(file, ".png");
    } else {
        info.detail = "No save present in this slot.";
    }
    const auto key = [this](const char* id) {
        const std::string k = m_hotkeys.key_for(id);
        return k.empty() ? std::string("—") : k;
    };
    info.legend = key("load-selected") + " Load   " + key("save-selected") + " Save   " +
                  key("prev-slot") + " / " + key("next-slot") + " Select";
    m_osd->show_slot(info);
}

void MainWindow::select_slot(int delta) {
    // Cycle through the 10 game slots, then the 10 global slots.
    int index = (m_selected_global ? kSlots : 0) + (m_selected_slot - 1);
    index = ((index + delta) % (2 * kSlots) + 2 * kSlots) % (2 * kSlots);
    m_selected_global = index >= kSlots;
    m_selected_slot   = index % kSlots + 1;
    show_slot_osd();
}

void MainWindow::rebuild_state_menus() {
    if (!m_load_menu || !m_save_menu)
        return;
    m_load_menu->remove_all();
    m_save_menu->remove_all();

    const auto fill = [this](const Glib::RefPtr<Gio::Menu>& menu, bool global, const char* action) {
        auto section = Gio::Menu::create();
        for (int slot = 1; slot <= kSlots; ++slot) {
            const fs::path file = slot_path(global, slot);
            const std::string when = fs::exists(file) ? timestamp_of(file) : "Empty";
            section->append(std::string(global ? "Global Save " : "Game Save ") +
                                std::to_string(slot) + " (" + when + ")",
                            std::string("win.") + action + "::" +
                                (global ? "global:" : "game:") + std::to_string(slot));
        }
        menu->append_section({}, section);
    };

    auto load_top = Gio::Menu::create();
    load_top->append("Load From File…", "win.load-slot::file");
    load_top->append("Undo Load State", "win.undo-load-state");
    m_load_menu->append_section({}, load_top);
    auto save_top = Gio::Menu::create();
    save_top->append("Save To File…", "win.save-slot::file");
    m_save_menu->append_section({}, save_top);

    if (m_emu_running && !m_current_game_path.empty()) {
        fill(m_load_menu, false, "load-slot");
        fill(m_save_menu, false, "save-slot");
    }
    fill(m_load_menu, true, "load-slot");
    fill(m_save_menu, true, "save-slot");
}

// ── Screenshot ────────────────────────────────────────────────────────────

void MainWindow::take_screenshot() {
    auto shot = m_emu_running ? m_render_widget->capture_frame() : Glib::RefPtr<Gdk::Pixbuf>{};
    if (!shot) {
        show_toast("Nothing to capture yet");
        return;
    }
    const fs::path file = Config::screenshots_dir() /
        (game_id_for(m_current_game_path) + " " + now_for_filename() + ".png");
    try {
        if (!image::save_png(shot, file))
            throw Glib::FileError(Glib::FileError::FAILED, "cannot write " + file.string());
        show_toast("Screenshot saved: " + file.filename().string());
    } catch (const Glib::Error& e) {
        show_toast(Glib::ustring("Could not save screenshot: ") + e.what());
    }
}

// ── Pause / close / launch ────────────────────────────────────────────────

void MainWindow::set_paused(bool paused) {
    if (paused && m_netplay)
        return;                 // the other side would be left waiting
    if (m_emu_running && m_act_pause)
        m_act_pause->change_state(Glib::Variant<bool>::create(paused));
}

void MainWindow::close_game_now(bool save_resume) {
    if (!m_emu_running)
        return;
    if (save_resume && !m_current_game_path.empty() && !m_netplay)
        save_state_file(resume_path(m_game_id));
    if (is_fullscreen())
        toggle_fullscreen();
    stop_emulation();
    set_game_visible(false);
    m_status_game_lbl.set_text("No game running");
    m_status_fps_lbl.set_text("");
    m_status_video_lbl.set_text("");
    m_status_scale_lbl.set_text("");
    m_views.set_visible_child_name("library");
}

void MainWindow::request_close_game(std::function<void()> after, bool allow_save) {
    if (!m_emu_running) {
        if (after) after();
        return;
    }
    const bool can_save = allow_save && !m_current_game_path.empty() && !m_netplay;
    if (!m_config.behavior.confirm_close) {
        close_game_now(can_save && m_config.behavior.save_state_on_close);
        if (after) after();
        return;
    }

    // Hold the game still while the question is on screen.
    const bool was_paused = m_render_widget->paused();
    set_paused(true);

    auto* save_check = Gtk::make_managed<Gtk::CheckButton>("Save State For Resume");
    save_check->set_active(can_save && m_config.behavior.save_state_on_close);
    save_check->set_sensitive(can_save);
    save_check->set_halign(Gtk::Align::CENTER);

    alert(*this, "Close Game", "Are you sure you want to close the current game?",
          {{"cancel", "No"}, {"close", "Yes"}}, "close", save_check,
          [this, save_check, was_paused, after](const std::string& response) {
              if (response != "close") {
                  if (!was_paused) set_paused(false);
                  return;
              }
              close_game_now(save_check->get_active());
              if (after) after();
          });
}

void MainWindow::launch(const fs::path& game, bool pick_disc) {
    if (m_emu_running) {
        request_close_game([this, game, pick_disc] { launch(game, pick_disc); });
        return;
    }
    // A merged multi-disc entry: ask which disc to boot.
    if (pick_disc && m_config.library.merge_discs) {
        if (const GameEntry* entry = m_library.find(game)) {
            const auto discs = m_library.set_members(*entry);
            if (discs.size() > 1) {
                static std::vector<std::string> ids, labels;   // storage for the C strings
                ids.clear(); labels.clear();
                for (const GameEntry* d : discs) {
                    ids.push_back("disc" + std::to_string(ids.size()));
                    labels.push_back("Disc " + std::to_string(d->disc));
                }
                // The dialog stacks buttons bottom-up: add the last disc first
                // so Disc 1 ends up on top.
                Responses responses{{"cancel", "Cancel"}};
                for (std::size_t i = discs.size(); i-- > 0;)
                    responses.push_back({ids[i].c_str(), labels[i].c_str()});
                std::vector<fs::path> paths;
                for (const GameEntry* d : discs) paths.push_back(d->path);
                alert(*this, entry->set_title, "Select the disc that you want to boot.",
                      responses, ids[0].c_str(), nullptr,
                      [this, paths](const std::string& response) {
                          if (response.rfind("disc", 0) != 0)
                              return;
                          const auto index = static_cast<std::size_t>(std::atoi(response.c_str() + 4));
                          if (index < paths.size())
                              launch(paths[index], false);
                      });
                return;
            }
        }
    }
    const fs::path resume = resume_path(game_id_for(game));
    if (!fs::exists(resume)) {
        start_emulation(game);
        return;
    }

    Gtk::Widget* preview = nullptr;
    if (const fs::path png = sidecar(resume, ".png"); fs::exists(png)) {
        auto* picture = Gtk::make_managed<Gtk::Picture>();
        picture->set_filename(png.string());
        picture->set_size_request(256, 192);
        picture->set_content_fit(Gtk::ContentFit::CONTAIN);
        preview = picture;
    }
    alert(*this, "Resume Game",
          "Do you want to load this state, or start from a fresh boot?\n"
          "Save was created on " + timestamp_of(resume) + ".",
          {{"cancel", "Cancel"}, {"delete", "Delete And Boot"}, {"fresh", "Fresh Boot"},
           {"load", "Load State"}},
          "load", preview,
          [this, game, resume](const std::string& response) {
              if (response == "cancel")
                  return;
              if (response == "delete") {
                  std::error_code ec;
                  for (const char* ext : {".sav", ".png", ".game"})
                      fs::remove(sidecar(resume, ext), ec);
              }
              start_emulation(game);
              if (response == "load")
                  m_render_widget->load_state_after_boot(resume);
          });
}

// ── Hotkeys ───────────────────────────────────────────────────────────────

void MainWindow::handle_hotkey(const std::string& id, bool pressed) {
    // Online, nothing may change the game's pace or state on one side only.
    if (m_netplay && pressed &&
        (id == "fast-forward" || id == "toggle-fast-forward" || id == "frame-step" ||
         id == "speed-up" || id == "speed-down" || id == "speed-reset" || id == "restart-game" ||
         id == "toggle-widescreen" || id == "toggle-pause")) {
        netplay_blocks("Not available during online play");
        return;
    }
    if (id == "fast-forward") {                 // hold
        if (m_emu_running)
            m_render_widget->set_fast_forward(pressed);
        return;
    }
    if (!pressed)
        return;

    if (id == "toggle-fullscreen") {
        toggle_fullscreen();
    } else if (id == "pause-menu") {
        open_pause_menu();
    } else if (id == "toggle-pause") {
        set_paused(!m_render_widget->paused());
    } else if (id == "screenshot") {
        take_screenshot();
    } else if (id == "toggle-fast-forward") {
        m_render_widget->set_fast_forward(!m_render_widget->fast_forward());
        show_toast(m_render_widget->fast_forward() ? "Fast forward on" : "Fast forward off");
    } else if (id == "frame-step") {
        // Pauses first, then advances one frame per press.
        if (!m_render_widget->paused()) set_paused(true);
        else                            m_render_widget->step_frame();
    } else if (id == "speed-up" || id == "speed-down" || id == "speed-reset") {
        static const int kSteps[] = {25, 50, 75, 100, 125, 150, 200, 300, 400};
        int index = 3;
        for (int i = 0; i < static_cast<int>(std::size(kSteps)); ++i)
            if (kSteps[i] <= m_config.display.speed) index = i;
        if (id == "speed-up")        index = std::min<int>(index + 1, std::size(kSteps) - 1);
        else if (id == "speed-down") index = std::max(index - 1, 0);
        else                         index = 3;
        m_config.display.speed = kSteps[index];
        m_config.save();
        apply_display_config();
        show_toast("Emulation speed: " + std::to_string(m_config.display.speed) + "%");
    } else if (id == "close-game") {
        on_power_off();
    } else if (id == "restart-game") {
        on_reset();
    } else if (id == "toggle-shader") {
        m_act_post->change_state(Glib::Variant<bool>::create(!m_config.post_enabled));
        show_toast(m_config.post_enabled ? "Post-processing on" : "Post-processing off");
    } else if (id == "toggle-widescreen") {
        m_act_widescreen->change_state(Glib::Variant<Glib::ustring>::create(
            widescreen_mode() == "off" ? "hack" : "off"));
    } else if (id == "resolution-up" || id == "resolution-down") {
        const int scale = static_cast<int>(m_config.graphics.internal_scale) +
                          (id == "resolution-up" ? 1 : -1);
        if (scale >= 1 && scale <= 8)
            m_act_resolution->change_state(
                Glib::Variant<Glib::ustring>::create(std::to_string(scale)));
    } else if (id == "toggle-mute") {
        m_config.audio.muted = !m_config.audio.muted;
        m_config.save();
        apply_audio_config();
        show_toast(m_config.audio.muted ? "Muted" : "Unmuted");
    } else if (id == "volume-up" || id == "volume-down") {
        m_config.audio.volume = std::clamp(m_config.audio.volume + (id == "volume-up" ? 10 : -10), 0, 200);
        m_config.save();
        apply_audio_config();
        show_toast("Volume: " + std::to_string(m_config.audio.volume) + "%");
    } else if (id == "load-selected") {
        load_slot(m_selected_global, m_selected_slot);
    } else if (id == "save-selected") {
        save_slot(m_selected_global, m_selected_slot);
    } else if (id == "prev-slot") {
        select_slot(-1);
    } else if (id == "next-slot") {
        select_slot(+1);
    } else if (id == "undo-load-state") {
        undo_load_state();
    }
}

// ── Disc changes ──────────────────────────────────────────────────────────

void MainWindow::rebuild_disc_menu() {
    if (!m_disc_menu)
        return;
    m_disc_menu->remove_all();
    auto top = Gio::Menu::create();
    top->append("From File…",  "win.change-disc::file");
    top->append("Remove Disc", "win.change-disc::remove");
    m_disc_menu->append_section({}, top);
    // The other discs of the running game, straight from the game list.
    if (m_emu_running) {
        if (const GameEntry* entry = m_library.find(m_current_game_path)) {
            const auto discs = m_library.set_members(*entry);
            if (discs.size() > 1) {
                auto set = Gio::Menu::create();
                for (const GameEntry* d : discs)
                    set->append(d->title, "win.change-disc::" + d->path.string());
                m_disc_menu->append_section({}, set);
            }
        }
    }
}

void MainWindow::change_disc(const std::string& target) {
    if (!m_emu_running || netplay_blocks("Changing discs is off during online play"))
        return;
    if (target == "remove") {
        show_toast(m_render_widget->open_tray() ? "Disc removed" : "Could not open the disc tray");
        return;
    }
    if (target == "file") {
        auto dlg = Gtk::FileDialog::create();
        dlg->set_title("Change Disc");
        auto filter = Gtk::FileFilter::create();
        filter->set_name("Dreamcast disc images");
        for (const char* pat : {"*.gdi", "*.cdi", "*.chd", "*.cue"})
            filter->add_pattern(pat);
        auto filters = Gio::ListStore<Gtk::FileFilter>::create();
        filters->append(filter);
        dlg->set_filters(filters);
        if (!m_current_game_path.empty())
            dlg->set_initial_folder(
                Gio::File::create_for_path(m_current_game_path.parent_path().string()));
        dlg->open(*this, [this, dlg](const Glib::RefPtr<Gio::AsyncResult>& res) {
            try {
                change_disc(dlg->open_finish(res)->get_path());
            } catch (...) {}
        });
        return;
    }
    const fs::path image = target;
    if (!fs::exists(image)) {
        show_toast("Disc image not found: " + image.filename().string());
        return;
    }
    std::string label = image.filename().string();
    if (const GameEntry* entry = m_library.find(image))
        label = entry->title;
    show_toast(m_render_widget->change_disc(image) ? "Disc changed to " + label
                                                   : "Could not change the disc");
}

// ── Audio ─────────────────────────────────────────────────────────────────

void MainWindow::apply_audio_config() {
    if (m_render_widget)
        m_render_widget->set_audio(m_config.audio.volume, m_config.audio.muted,
                                   m_config.audio.device, m_config.audio.buffer_ms);
}

void MainWindow::apply_display_config() {
    if (!m_render_widget)
        return;
    m_render_widget->set_display(m_config.display.aspect, m_config.display.filter == "nearest",
                                 m_config.display.integer);
    // Online both sides run at full speed.
    const int speed = m_netplay ? 100 : m_config.display.speed;
    if (m_render_widget->speed() != speed)
        m_render_widget->set_speed(speed);
}

// ── Gamepad hotkeys ───────────────────────────────────────────────────────

bool MainWindow::poll_pad_hotkeys() {
    const unsigned now = PadInput::instance().held_buttons();
    const unsigned prev = m_pad_buttons_prev;
    m_pad_buttons_prev = now;
    if (prev == ~0u || now == prev)
        return true;
    const bool game_view = m_emu_running && m_views.get_visible_child_name() == "game";
    for (int button = 0; button < 32; ++button) {
        const unsigned bit = 1u << button;
        if (!((now ^ prev) & bit))
            continue;
        const HotkeyDef* hk = m_hotkeys.lookup_pad(button);
        if (!hk || !game_view)
            continue;
        const bool pressed = (now & bit) != 0;
        const std::string id = hk->id;
        // With the pause menu up, its own key closes it; nothing else fires.
        if (m_pause_menu->is_open()) {
            if (pressed && id == "pause-menu" && !m_pause_menu->go_back())
                close_pause_menu(true);
            continue;
        }
        if (pressed || hk->hold)
            handle_hotkey(id, pressed);
    }
    return true;
}

// ── Pause menu ────────────────────────────────────────────────────────────

void MainWindow::open_pause_menu() {
    if (!m_emu_running || m_pause_menu->is_open())
        return;
    m_menu_paused_game = !m_render_widget->paused();
    if (m_menu_paused_game)
        set_paused(true);

    PauseMenu::Info info;
    info.title           = m_current_game_title;
    info.subtitle        = m_current_game_path.filename().string();
    info.session_time    = PlayStats::format_duration(m_session_seconds);
    info.total_time      = PlayStats::format_duration(m_stats.get(m_game_id).seconds + m_session_seconds);
    info.can_save_states = !m_current_game_path.empty();
    info.fast_forward    = m_render_widget->fast_forward();
    if (const GameEntry* entry = m_library.find(m_current_game_path)) {
        const auto discs = m_library.set_members(*entry);
        if (discs.size() > 1)
            for (const GameEntry* d : discs)
                info.discs.emplace_back(d->title, d->path.string());
    }
    info.slot = [this](bool global, int slot) {
        PauseMenu::Slot s;
        const fs::path file = slot_path(global, slot);
        s.exists = fs::exists(file);
        if (s.exists) {
            s.when       = timestamp_of(file);
            s.screenshot = sidecar(file, ".png");
        }
        return s;
    };
    m_pause_menu->open(std::move(info));
    update_cursor_and_inhibit();
}

void MainWindow::close_pause_menu(bool resume) {
    if (!m_pause_menu->is_open())
        return;
    m_pause_menu->close_menu();
    if (resume && m_menu_paused_game)
        set_paused(false);
    m_menu_paused_game = false;
    m_render_widget->grab_focus();
    update_cursor_and_inhibit();
}

void MainWindow::on_pause_menu_action(const std::string& action) {
    if (action == "resume") {
        close_pause_menu(true);
    } else if (action == "fast-forward") {
        close_pause_menu(true);
        handle_hotkey("toggle-fast-forward", true);
    } else if (action == "screenshot") {
        take_screenshot();
    } else if (action == "properties") {
        open_game_properties(m_current_game_path);
    } else if (action == "restart") {
        close_pause_menu(true);
        on_reset();
    } else if (action == "close-save" || action == "close-nosave") {
        close_pause_menu(false);
        close_game_now(action == "close-save");
    } else if (action.rfind("disc:", 0) == 0) {
        close_pause_menu(true);
        change_disc(action.substr(5));
    } else if (action.rfind("load:", 0) == 0 || action.rfind("save:", 0) == 0) {
        const bool save = action[0] == 's';
        close_pause_menu(true);
        on_state_action(action.substr(5), save);
    }
}

// ── Cursor and screensaver ────────────────────────────────────────────────

void MainWindow::update_cursor_and_inhibit() {
    const bool playing = m_emu_running && !m_render_widget->paused();
    const bool on_game = m_views.get_visible_child_name() == "game";
    m_render_widget->set_cursor_hidden(playing && on_game && is_fullscreen() &&
                                       !(m_pause_menu && m_pause_menu->is_open()) &&
                                       m_config.behavior.hide_cursor_fullscreen);

    auto app = get_application();
    if (!app)
        return;
    const bool want = playing && m_config.behavior.inhibit_screensaver;
    if (want && m_inhibit_cookie == 0) {
        m_inhibit_cookie = app->inhibit(*this, Gtk::Application::InhibitFlags::IDLE,
                                        "A game is running");
    } else if (!want && m_inhibit_cookie != 0) {
        app->uninhibit(m_inhibit_cookie);
        m_inhibit_cookie = 0;
    }
}

// ── Game list actions ─────────────────────────────────────────────────────

void MainWindow::flush_play_time() {
    if (m_session_seconds > 0 && !m_game_id.empty() && !m_current_game_path.empty()) {
        m_stats.add_seconds(m_game_id, m_session_seconds);
        m_stats.touch(m_game_id);
        m_stats.save();
    }
    m_session_seconds = 0;
}

void MainWindow::on_game_action(const std::string& action, const fs::path& game) {
    const std::string id = game_id_for(game);
    if (action == "properties") {
        open_game_properties(game);
    } else if (action == "open-dir") {
        try {
            Gio::AppInfo::launch_default_for_uri(
                Glib::filename_to_uri(game.parent_path().string()));
        } catch (const Glib::Error&) {
            show_toast("Could not open " + game.parent_path().string());
        }
    } else if (action == "set-cover") {
        auto dlg = Gtk::FileDialog::create();
        dlg->set_title("Select Cover Image");
        auto filter = Gtk::FileFilter::create();
        filter->set_name("Images");
        for (const char* pat : {"*.png", "*.jpg", "*.jpeg", "*.webp"})
            filter->add_pattern(pat);
        auto filters = Gio::ListStore<Gtk::FileFilter>::create();
        filters->append(filter);
        dlg->set_filters(filters);
        dlg->open(*this, [this, dlg, game](const Glib::RefPtr<Gio::AsyncResult>& res) {
            try {
                const fs::path src = dlg->open_finish(res)->get_path();
                // Named after the file (or its folder for disc.gdi) so the
                // cover lookup finds it first, in our own covers folder.
                std::string name = game.stem().string();
                if (GameLibrary::is_generic_name(name))
                    name = game.parent_path().filename().string();
                std::error_code ec;
                for (const char* ext : {".png", ".jpg", ".jpeg", ".webp"})
                    fs::remove(Config::covers_dir() / (name + ext), ec);
                fs::create_directories(Config::covers_dir(), ec);
                fs::copy_file(src, Config::covers_dir() / (name + src.extension().string()),
                              fs::copy_options::overwrite_existing, ec);
                if (ec) show_toast("Could not copy the cover image");
                else    m_library_page->refresh();
            } catch (...) {}
        });
    } else if (action == "play-online") {
        open_netplay_dialog(game);
    } else if (action == "fresh-boot") {
        const auto boot = [this, game] { start_emulation(game); };
        if (m_emu_running) request_close_game(boot);
        else               boot();
    } else if (action == "delete-states") {
        std::error_code ec;
        int removed = 0;
        for (const auto& e : fs::directory_iterator(Config::savestates_dir(), ec)) {
            const std::string name = e.path().filename().string();
            if (name.rfind(id + "_", 0) == 0) {
                fs::remove(e.path(), ec);
                ++removed;
            }
        }
        rebuild_state_menus();
        show_toast(removed ? "Save states deleted" : "This game has no save states");
    } else if (action == "exclude") {
        m_config.paths.excluded.push_back(game);
        m_config.save();
        rescan_library();
        show_toast("Excluded from the list (undo in Settings → Game List)");
    } else if (action == "reset-playtime") {
        m_stats.reset(id);
        m_stats.save();
        m_library_page->refresh();
    }
}

// ── Developer aid: scripted UI runs ───────────────────────────────────────
//
// DS_TEST_SCRIPT="<ms>:<step>;<ms>:<step>;..." runs steps at the given times
// after start-up so behaviour can be checked without a person at the screen:
//   hotkey:<id>            press a hotkey action (see hotkeys.cpp)
//   action:<name>[=<arg>]  activate a window action, optional string target
//   respond:<id>           answer the open prompt (e.g. load, fresh, close)
//   shot:<file.png>        save an image of the whole window
//   shot-settings:<file>   ... of the settings window (shot-game-settings: per-game)
//   launch:<path>          start a game, as the game list would
//   vmu-import:<L|R>:<files> / vmu-select-all / vmu-copy / vmu-move / vmu-save:<L|R>
//                          drive the VMU manager (open it with action:vmu-editor)
//   key:<gdk name>         tap a keyboard key that is bound to the pad
//   quit                   close the application

void MainWindow::save_window_image(const std::string& file, Gtk::Window* window) {
    if (!window)
        window = this;
    GtkWidget*    self      = GTK_WIDGET(window->gobj());
    GdkPaintable* paintable = gtk_widget_paintable_new(self);
    GtkSnapshot*  snap      = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snap), window->get_width(), window->get_height());
    if (GskRenderNode* node = gtk_snapshot_free_to_node(snap)) {
        GskRenderer* renderer = gtk_native_get_renderer(GTK_NATIVE(self));
        GdkTexture*  texture  = gsk_renderer_render_texture(renderer, node, nullptr);
        if (!gdk_texture_save_to_png(texture, file.c_str()))
            std::fprintf(stderr, "test-script: could not save %s (%dx%d)\n", file.c_str(),
                         gdk_texture_get_width(texture), gdk_texture_get_height(texture));
        g_object_unref(texture);
        gsk_render_node_unref(node);
    } else {
        // Nothing drawn, e.g. the window is on a workspace not being shown.
        std::fprintf(stderr, "test-script: could not save %s (window has no content)\n",
                     file.c_str());
    }
    g_object_unref(paintable);
}

void MainWindow::run_test_script() {
    const char* script = std::getenv("DS_TEST_SCRIPT");
    if (!script)
        return;
    std::string text = script;
    m_test_start_ms = g_get_monotonic_time() / 1000;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t end = std::min(text.find(';', pos), text.size());
        const std::string entry = text.substr(pos, end - pos);
        pos = end + 1;
        const auto colon = entry.find(':');
        if (colon == std::string::npos)
            continue;
        const unsigned at = static_cast<unsigned>(std::atoi(entry.substr(0, colon).c_str()));
        const std::string step = entry.substr(colon + 1);
        Glib::signal_timeout().connect_once([this, step] {
            std::fprintf(stderr, "test-script: %s\n", step.c_str());
            if (step == "quit") {
                m_closing = true;
                close_game_now(false);
                if (auto app = get_application()) app->quit();
            } else if (step.rfind("respond:", 0) == 0) {
                if (AdwDialog* dialog = g_open_alert) {
                    g_signal_emit_by_name(dialog, "response", step.substr(8).c_str());
                    adw_dialog_force_close(dialog);
                }
            } else if (step.rfind("hotkey:", 0) == 0) {
                handle_hotkey(step.substr(7), true);
            } else if (step.rfind("shot:", 0) == 0) {
                save_window_image(step.substr(5));
            } else if (step.rfind("shot-covers:", 0) == 0) {
                if (m_cover_downloader) save_window_image(step.substr(12), m_cover_downloader.get());
            } else if (step == "cover-report") {
                // Every game and the cover the library picks for it.
                const auto dirs = cover_dirs();
                for (const auto& g : m_library.games()) {
                    const auto cover = find_cover(g, dirs);
                    const auto box = find_art(g, true);
                    std::fprintf(stderr, "cover-report: %s\t%s\t%s\t%s\t%s\t%s\n",
                                 cover.empty() ? "MISSING" : "ok", box.string().find("/3dboxes/") == std::string::npos ? "no-3d" : "3d",
                                 g.system.c_str(), g.title.c_str(), cover.c_str(), box.c_str());
                }
            } else if (step.rfind("netplay-host:", 0) == 0 || step.rfind("netplay-join:", 0) == 0) {
                // netplay-host:<game> or netplay-join:<address>|<game>
                ds::engine::Engine::Netplay netplay;
                netplay.host = step.rfind("netplay-host:", 0) == 0;
                std::string rest = step.substr(13);
                if (!netplay.host) {
                    const auto bar = rest.find('|');
                    netplay.peer = rest.substr(0, bar);
                    rest = bar == std::string::npos ? "" : rest.substr(bar + 1);
                }
                start_emulation(rest, false, netplay);
            } else if (step.rfind("cover-size:", 0) == 0) {
                m_library_page->test_cover_size(std::atoi(step.c_str() + 11));
            } else if (step == "covers-shown") {
                const auto [shown, tiles] = m_library_page->covers_shown();
                std::fprintf(stderr, "covers-shown: %d of %d at %lld ms\n", shown, tiles,
                             static_cast<long long>(g_get_monotonic_time() / 1000 - m_test_start_ms));
            } else if (step == "covers-start") {
                if (m_cover_downloader) m_cover_downloader->start();
            } else if (step.rfind("launch:", 0) == 0) {
                launch(step.substr(7), false);
            } else if (step == "frame-status") {
                std::fprintf(stderr, "frame-status: emulated %llu paused %d speed %d%% vmu-a1 %d\n",
                             static_cast<unsigned long long>(m_render_widget->frames_emulated()),
                             m_render_widget->paused(), m_render_widget->speed(),
                             ds::engine::Engine::instance().vmu_present(0, 0));
            } else if (step == "disc-status") {
                std::fprintf(stderr, "disc-status: %s\n",
                             ds::engine::Engine::instance().current_disc().string().c_str());
            } else if (step.rfind("menu:", 0) == 0) {
                // Drive the pause menu as a click would.
                on_pause_menu_action(step.substr(5));
            } else if (step.rfind("key:", 0) == 0) {
                // Tap a keyboard key bound to the pad (held for 150 ms).
                const guint keyval = gdk_keyval_from_name(step.substr(4).c_str());
                m_render_widget->handle_key(keyval, true);
                Glib::signal_timeout().connect_once(
                    [this, keyval] { m_render_widget->handle_key(keyval, false); }, 150);
            } else if (step.rfind("shot-top:", 0) == 0) {
                // Whatever window is on top of the application (e.g. a picker).
                for (auto* w : Gtk::Window::list_toplevels())
                    if (w != this && w->is_visible() && w->get_title() == "Select Shader")
                        save_window_image(step.substr(9), w);
            } else if (step.rfind("vmu-", 0) == 0 && m_vmu_manager) {
                // vmu-import:<L|R>:<path>[|<path>...], vmu-select-all:<L|R>,
                // vmu-copy:<L|R>, vmu-move:<L|R>, vmu-save:<L|R>
                const auto sep = step.find(':');
                const std::string verb = step.substr(4, sep - 4);
                const int side = sep + 1 < step.size() && step[sep + 1] == 'R';
                if (verb == "import") {
                    std::vector<fs::path> paths;
                    std::string rest = step.size() > sep + 3 ? step.substr(sep + 3) : "";
                    for (std::size_t from = 0; from <= rest.size();) {
                        const std::size_t bar = std::min(rest.find('|', from), rest.size());
                        if (bar > from) paths.emplace_back(rest.substr(from, bar - from));
                        from = bar + 1;
                    }
                    m_vmu_manager->test_import(side, paths);
                } else if (verb == "select-all") {
                    m_vmu_manager->test_select_all(side);
                } else if (verb == "copy" || verb == "move") {
                    m_vmu_manager->test_transfer(side, verb == "move");
                } else if (verb == "save") {
                    m_vmu_manager->test_save(side);
                }
            } else if (step.rfind("vmu-hold:", 0) == 0) {
                // Hold VMU buttons in the player (hex VmuCpu::Button bits; 0 releases).
                const auto bits = static_cast<unsigned>(std::strtoul(step.c_str() + 9, nullptr, 16));
                if (m_vmu_player)
                    m_vmu_player->test_hold(bits);
                if (m_vmu_window)
                    m_vmu_window->test_hold(bits);
            } else if (step.rfind("vmu-window-toggle:", 0) == 0) {
                std::string error;
                if (!m_vmu_window || !m_vmu_window->test_toggle(std::atoi(step.c_str() + 18), error))
                    std::fprintf(stderr, "test-script: vmu-window-toggle failed: %s\n", error.c_str());
            } else if (step.rfind("shot-vmu-window:", 0) == 0) {
                if (m_vmu_window) save_window_image(step.substr(16), m_vmu_window.get());
            } else if (step.rfind("shot-vmu-player:", 0) == 0) {
                if (m_vmu_player) save_window_image(step.substr(16), m_vmu_player.get());
            } else if (step.rfind("shot-vmu:", 0) == 0) {
                if (m_vmu_manager) save_window_image(step.substr(9), m_vmu_manager.get());
            } else if (step.rfind("shot-controllers:", 0) == 0) {
                if (m_controllers_win) save_window_image(step.substr(17), m_controllers_win.get());
            } else if (step.rfind("settings-page:", 0) == 0) {
                open_settings(step.substr(14));
            } else if (step.rfind("shot-netplay:", 0) == 0) {
                if (m_netplay_dialog) save_window_image(step.substr(13), m_netplay_dialog.get());
            } else if (step.rfind("shot-settings:", 0) == 0) {
                if (m_settings_win) save_window_image(step.substr(14), m_settings_win.get());
            } else if (step.rfind("shot-game-settings:", 0) == 0) {
                if (m_game_settings_win) save_window_image(step.substr(19), m_game_settings_win.get());
            } else if (step.rfind("action:", 0) == 0) {
                const std::string rest = step.substr(7);
                const auto eq = rest.find('=');
                if (eq == std::string::npos)
                    m_action_group->activate_action(rest);
                else
                    m_action_group->activate_action(
                        rest.substr(0, eq),
                        Glib::Variant<Glib::ustring>::create(rest.substr(eq + 1)));
            }
        }, at);
    }
}

} // namespace ds::frontend
