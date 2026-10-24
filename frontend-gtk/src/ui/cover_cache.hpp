// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gtkmm.h>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../game_library.hpp"

namespace ds::frontend {

// Makes covers cheap to show again, so the game list appears at once instead
// of decoding every full-size image on each start and redraw:
//  - which file is a game's cover is remembered until forget();
//  - each cover is shrunk once to a thumbnail kept on disk (in the cache
//    directory, named after the source file's path, time and size, so an
//    edited cover gets a new one);
//  - thumbnails are decoded on worker threads, never on the UI thread;
//  - the last picture made of each cover stays in memory for redraws.
class CoverCache {
public:
    using Done = std::function<void(const Glib::RefPtr<Gdk::Texture>&)>;

    CoverCache();
    ~CoverCache();
    CoverCache(const CoverCache&) = delete;
    CoverCache& operator=(const CoverCache&) = delete;

    // The game's cover file, empty if it has none.
    std::filesystem::path path_for(const GameEntry& game);
    // Covers were added or replaced: look them up again.
    void forget();

    // The picture last made of `file`, at whatever size (null if none or the
    // file changed since). `exact` is set when it is `px` pixels already.
    Glib::RefPtr<Gdk::Texture> lookup(const std::filesystem::path& file, int px, bool& exact);
    // Makes the picture of `file` fitting `px` x `px` in the background and
    // calls `done` with it on the UI thread (not if it cannot be read).
    // Requests for another size than the last are dropped unless started.
    void request(const std::filesystem::path& file, int px, Done done);
    // Pictures as flat covers or as 3D game cases (see box_art.hpp). A
    // change drops the pictures made in the other style.
    void set_box_art(bool on);

private:
    struct Job {
        std::filesystem::path file;
        int px = 0;
        bool box = false;
    };
    struct Result {
        std::filesystem::path file;
        int px = 0;
        bool box = false;
        GdkPixbuf* pixbuf = nullptr;   // owned; null if unreadable
    };
    struct Entry {
        int px = 0;
        std::filesystem::file_time_type time;
        std::uintmax_t size = 0;
        Glib::RefPtr<Gdk::Texture> texture;
    };

    void worker();
    void deliver();
    GdkPixbuf* make(const Job& job) const;
    GdkPixbuf* make_flat(const Job& job, const std::string& name) const;

    std::filesystem::path m_dir;   // thumbnails on disk
    std::unordered_map<std::string, std::filesystem::path> m_paths;
    std::unordered_map<std::string, Entry> m_textures;
    std::map<std::pair<std::string, int>, std::vector<Done>> m_waiting;
    int m_px = 0;                  // size of the latest requests
    bool m_box = false;            // pictures as 3D cases

    std::mutex              m_mutex;   // guards the two queues and m_stop
    std::condition_variable m_wake;
    std::deque<Job>         m_jobs;
    std::deque<Result>      m_results;
    bool                    m_stop = false;
    Glib::Dispatcher        m_dispatcher;
    std::vector<std::thread> m_threads;
};

} // namespace ds::frontend
