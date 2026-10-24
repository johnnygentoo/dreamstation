// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gtkmm.h>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../game_library.hpp"

namespace ds::frontend {

struct Config;

// Downloads cover art for games that have none, DuckStation-style: the user
// supplies URL templates (one per line) which are tried in order per game.
//   ${title}      cleaned display title
//   ${filetitle}  file name without extension (folder name for disc.gdi)
//   ${serial}     product number
// Arcade games are looked up instead in libretro's thumbnail listing for their
// board, matched by title, since those names follow MAME and not the files.
// Dreamcast games the templates miss get the same treatment. A game with no
// box art there gets its title screen.
class CoverDownloader : public Gtk::Window {
public:
    CoverDownloader(Config& config, const GameLibrary& library);
    ~CoverDownloader() override;

    // Fires each time a cover has been saved.
    sigc::signal<void()> signal_cover_saved;

    void start();
    void stop();
    bool running() const { return m_running; }

private:
    struct Job {
        GameEntry                game;
        std::vector<std::string> urls;   // remaining candidates
        bool                     listing = false;  // look in the system's listings when urls run out
        int                      kind = 0;         // next listing: box art, then title screens
    };

    void next();
    void try_download(Job job);
    void resolve_from_listing(Job job);
    void finish(const std::string& text);

    Config&            m_config;
    const GameLibrary& m_library;

    Gtk::Box         m_box{Gtk::Orientation::VERTICAL, 10};
    Gtk::TextView    m_templates;
    Gtk::Label       m_status;
    Gtk::ProgressBar m_progress;
    Gtk::Button      m_start{"Start"};

    std::deque<Job> m_queue;
    std::size_t     m_total   = 0;
    std::size_t     m_saved   = 0;
    bool            m_running = false;
    Glib::RefPtr<Gio::Subprocess> m_proc;
    std::shared_ptr<bool>         m_alive = std::make_shared<bool>(true);   // false once destroyed
    // Picture names available per "system/kind", fetched once per run.
    std::map<std::string, std::vector<std::string>> m_listings;
};

} // namespace ds::frontend
