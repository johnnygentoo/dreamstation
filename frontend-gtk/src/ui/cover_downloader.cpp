// SPDX-License-Identifier: GPL-3.0-or-later
#include "cover_downloader.hpp"
#include "../image_io.hpp"
#include "library_page.hpp"
#include "../config.hpp"

#include <cstring>
#include <sstream>

namespace ds::frontend {

namespace fs = std::filesystem;

namespace {

const char* const kDefaultTemplates =
    "https://thumbnails.libretro.com/Sega%20-%20Dreamcast/Named_Boxarts/${filetitle}.png\n"
    "https://thumbnails.libretro.com/Sega%20-%20Dreamcast/Named_Boxarts/${title}.png";

std::string substitute(std::string url, const std::string& var, const std::string& value) {
    const std::string token = "${" + var + "}";
    const std::string enc   = Glib::uri_escape_string(value);
    for (auto pos = url.find(token); pos != std::string::npos; pos = url.find(token, pos))
        url.replace(pos, token.size(), enc), pos += enc.size();
    return url;
}

// libretro's thumbnail folder for a system, URL-escaped; the kinds of
// picture in it, in order of preference (box art, else the title screen).
std::string thumbnail_base(const std::string& system) {
    std::string folder;
    if (system == "Dreamcast")       folder = "Sega%20-%20Dreamcast";
    else if (system == "Naomi")      folder = "Sega%20-%20Naomi";
    else if (system == "Naomi 2")    folder = "Sega%20-%20Naomi%202";
    else if (system == "Atomiswave") folder = "Atomiswave";
    else return {};
    return "https://thumbnails.libretro.com/" + folder + "/";
}
const char* const kKinds[] = {"Named_Boxarts", "Named_Titles"};

// File names (without .png) linked from a server's directory listing.
std::vector<std::string> listed_pngs(const std::string& html) {
    std::vector<std::string> names;
    const std::string open = "href=\"";
    for (auto pos = html.find(open); pos != std::string::npos; pos = html.find(open, pos)) {
        pos += open.size();
        const auto end = html.find('"', pos);
        if (end == std::string::npos)
            break;
        const std::string href = html.substr(pos, end - pos);
        if (href.size() > 4 && href.compare(href.size() - 4, 4, ".png") == 0 &&
            href.find('/') == std::string::npos)
            names.push_back(Glib::uri_unescape_string(href.substr(0, href.size() - 4)));
    }
    return names;
}

std::string file_title(const GameEntry& g) {
    if (GameLibrary::is_generic_name(g.path.stem().string()))
        return g.path.parent_path().filename().string();
    return g.path.stem().string();
}

} // namespace

CoverDownloader::CoverDownloader(Config& config, const GameLibrary& library)
    : m_config(config), m_library(library) {
    set_title("Download Covers");
    set_default_size(720, 420);
    add_css_class("ds-settings");
    m_box.set_margin(16);

    auto* intro = Gtk::make_managed<Gtk::Label>(
        "Covers are fetched for games that have none. Enter one URL template per line; "
        "they are tried in order. Variables: ${title}, ${filetitle}, ${serial}.\n"
        "Covers are saved to " + Config::covers_dir().string());
    intro->set_wrap(true);
    intro->set_xalign(0.f);
    m_box.append(*intro);

    std::string text;
    for (const auto& u : m_config.library.cover_urls)
        text += u + "\n";
    if (text.empty())
        text = kDefaultTemplates;
    m_templates.get_buffer()->set_text(text);
    m_templates.set_monospace(true);
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_child(m_templates);
    scroll->set_vexpand(true);
    scroll->add_css_class("ds-settings-help");
    m_box.append(*scroll);

    m_status.set_xalign(0.f);
    m_status.set_text("Idle");
    m_box.append(m_status);
    m_box.append(m_progress);

    auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    buttons->set_halign(Gtk::Align::END);
    auto* close_btn = Gtk::make_managed<Gtk::Button>("Close");
    close_btn->signal_clicked().connect([this] { stop(); close(); });
    m_start.signal_clicked().connect([this] { if (m_running) stop(); else start(); });
    buttons->append(m_start);
    buttons->append(*close_btn);
    m_box.append(*buttons);
    set_child(m_box);
}

CoverDownloader::~CoverDownloader() {
    // A download may still be under way; its completion must not reach us.
    *m_alive = false;
    if (m_proc)
        m_proc->force_exit();
}

void CoverDownloader::start() {
    // Remember the templates.
    std::vector<std::string> templates;
    std::istringstream lines(m_templates.get_buffer()->get_text());
    for (std::string line; std::getline(lines, line);) {
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        if (!line.empty())
            templates.push_back(line);
    }
    // The defaults are not stored, so that changes to them reach users who
    // never edited the list.
    std::vector<std::string> stored = templates;
    {
        std::vector<std::string> defaults;
        std::istringstream def(kDefaultTemplates);
        for (std::string line; std::getline(def, line);)
            defaults.push_back(line);
        if (stored == defaults)
            stored.clear();
    }
    if (stored != m_config.library.cover_urls) {
        m_config.library.cover_urls = stored;
        m_config.save();
    }
    if (templates.empty()) {
        m_status.set_text("Enter at least one URL template");
        return;
    }

    m_queue.clear();
    m_listings.clear();
    const auto dirs = cover_dirs();
    for (const auto& g : m_library.games()) {
        if (!find_cover(g, dirs).empty())
            continue;
        Job job{g, {}};
        // Arcade covers are only found through the listing; for Dreamcast it
        // is the fallback when no template finds the file by name, since
        // libretro's names carry a region ("Carrier (USA)").
        job.listing = !thumbnail_base(g.system).empty();
        if (g.is_arcade()) {
            if (job.listing)
                m_queue.push_back(std::move(job));
            continue;
        }
        for (const auto& t : templates) {
            std::string url = substitute(t, "title", g.title);
            url = substitute(url, "filetitle", file_title(g));
            url = substitute(url, "serial", g.product_id);
            // A template needing a serial is useless for a game without one.
            if (t.find("${serial}") != std::string::npos && g.product_id.empty())
                continue;
            job.urls.push_back(std::move(url));
        }
        if (!job.urls.empty() || job.listing)
            m_queue.push_back(std::move(job));
    }
    m_total = m_queue.size();
    m_saved = 0;
    if (m_total == 0) {
        m_status.set_text("Every game already has a cover");
        return;
    }
    m_running = true;
    m_start.set_label("Stop");
    next();
}

void CoverDownloader::stop() {
    if (!m_running)
        return;
    m_queue.clear();
    if (m_proc)
        m_proc->force_exit();
    finish("Stopped");
}

void CoverDownloader::finish(const std::string& text) {
    m_running = false;
    m_proc.reset();
    m_start.set_label("Start");
    m_status.set_text(text + " — " + std::to_string(m_saved) + " of " +
                      std::to_string(m_total) + " covers saved");
}

void CoverDownloader::next() {
    if (!m_running)
        return;
    if (m_queue.empty()) {
        m_progress.set_fraction(1.0);
        finish("Done");
        return;
    }
    Job job = std::move(m_queue.front());
    m_queue.pop_front();
    m_progress.set_fraction(1.0 - static_cast<double>(m_queue.size() + 1) /
                                      static_cast<double>(m_total));
    m_status.set_text("Downloading cover for " + job.game.title + "…");
    try_download(std::move(job));
}

// Picks the game's cover from its system's listing by title, fetching the
// listing first if this run has not yet.
void CoverDownloader::resolve_from_listing(Job job) {
    const std::string kind = kKinds[job.kind];
    const std::string base = thumbnail_base(job.game.system) + kind + "/";
    const std::string key  = job.game.system + "/" + kind;
    const auto it = m_listings.find(key);
    if (it != m_listings.end()) {
        ++job.kind;
        job.listing = job.kind < static_cast<int>(std::size(kKinds));
        const GameEntry& g = job.game;
        const int i = match_cover_name(cover_title(g), it->second, g.region, g.disc);
        if (i >= 0)
            job.urls.push_back(base + Glib::uri_escape_string(it->second[static_cast<std::size_t>(i)]) + ".png");
        try_download(std::move(job));
        return;
    }
    m_status.set_text("Fetching the " + job.game.system + " picture list…");
    try {
        m_proc = Gio::Subprocess::create({"curl", "-fsSL", "--max-time", "25", base},
            Gio::Subprocess::Flags::STDOUT_PIPE | Gio::Subprocess::Flags::STDERR_SILENCE);
    } catch (const Glib::Error& e) {
        finish(std::string("Could not run curl: ") + e.what());
        return;
    }
    auto proc = m_proc;
    proc->communicate_utf8_async({}, [this, alive = m_alive, proc, job = std::move(job)](
                                         const Glib::RefPtr<Gio::AsyncResult>& res) mutable {
        if (!*alive)
            return;
        Glib::ustring out;
        try {
            out = proc->communicate_utf8_finish(res).first;
        } catch (const Glib::Error&) {
            // Offline or not valid text: no listing.
        }
        if (!m_running)
            return;
        // An empty list (offline) is remembered too: no point asking again.
        m_listings[job.game.system + "/" + kKinds[job.kind]] = listed_pngs(out);
        resolve_from_listing(std::move(job));
    });
}

void CoverDownloader::try_download(Job job) {
    if (job.urls.empty()) {
        if (job.listing)
            resolve_from_listing(std::move(job));
        else
            next();
        return;
    }
    const std::string url = job.urls.front();
    job.urls.erase(job.urls.begin());

    std::string ext = ".png";
    for (const char* e : {".jpg", ".jpeg", ".webp", ".png"})
        if (url.size() > std::strlen(e) && url.compare(url.size() - std::strlen(e), std::string::npos, e) == 0)
            ext = e;
    const fs::path dest = Config::covers_dir() / (file_title(job.game) + ext);
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);

    try {
        m_proc = Gio::Subprocess::create(
            {"curl", "-fsSL", "--max-time", "25", "-o", dest.string(), url},
            Gio::Subprocess::Flags::STDOUT_SILENCE | Gio::Subprocess::Flags::STDERR_SILENCE);
    } catch (const Glib::Error& e) {
        finish(std::string("Could not run curl: ") + e.what());
        return;
    }
    auto proc = m_proc;
    proc->wait_async([this, alive = m_alive, proc, job = std::move(job), dest](
                         const Glib::RefPtr<Gio::AsyncResult>& res) mutable {
        if (!*alive)
            return;
        bool ok = false;
        try {
            proc->wait_finish(res);
            ok = proc->get_if_exited() && proc->get_exit_status() == 0;
            int w = 0, h = 0;
            if (ok)
                ok = image::info(dest, w, h);                        // must be an image
        } catch (const Glib::Error&) {
            ok = false;
        }
        if (!m_running)
            return;
        if (ok) {
            ++m_saved;
            signal_cover_saved.emit();
            next();
        } else {
            std::error_code remove_ec;
            fs::remove(dest, remove_ec);
            try_download(std::move(job));
        }
    });
}

} // namespace ds::frontend
