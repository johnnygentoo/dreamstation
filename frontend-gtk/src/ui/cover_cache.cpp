// SPDX-License-Identifier: GPL-3.0-or-later
#include "cover_cache.hpp"
#include "box_art.hpp"
#include "../image_io.hpp"
#include "library_page.hpp"
#include "../config.hpp"

#include <algorithm>
#include <cstdio>

namespace ds::frontend {

namespace fs = std::filesystem;

namespace {

// Thumbnails are this many pixels on their longer edge: the largest tile.
// Bigger requests (a scaled display) are made from the original.
constexpr int kThumbEdge = 512;

bool stat_file(const fs::path& file, fs::file_time_type& time, std::uintmax_t& size) {
    std::error_code ec;
    time = fs::last_write_time(file, ec);
    if (ec) return false;
    size = fs::file_size(file, ec);
    return !ec;
}

// `pixbuf` shrunk to fit `px` x `px`; takes ownership of `pixbuf`.
GdkPixbuf* fit(GdkPixbuf* pixbuf, int px) {
    const int w = gdk_pixbuf_get_width(pixbuf), h = gdk_pixbuf_get_height(pixbuf);
    if (w <= px && h <= px)
        return pixbuf;
    const double scale = std::min(double(px) / w, double(px) / h);
    GdkPixbuf* out = gdk_pixbuf_scale_simple(pixbuf, std::max(1, int(w * scale + 0.5)),
                                             std::max(1, int(h * scale + 0.5)),
                                             GDK_INTERP_BILINEAR);
    g_object_unref(pixbuf);
    return out;
}

// `pixbuf` without its alpha channel if no pixel is see-through (many PNG
// covers carry one regardless); takes ownership of `pixbuf`.
GdkPixbuf* drop_unused_alpha(GdkPixbuf* pixbuf) {
    if (!gdk_pixbuf_get_has_alpha(pixbuf) || gdk_pixbuf_get_n_channels(pixbuf) != 4 ||
        gdk_pixbuf_get_bits_per_sample(pixbuf) != 8)
        return pixbuf;
    const int w = gdk_pixbuf_get_width(pixbuf), h = gdk_pixbuf_get_height(pixbuf);
    const int stride = gdk_pixbuf_get_rowstride(pixbuf);
    const guchar* src = gdk_pixbuf_read_pixels(pixbuf);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (src[y * stride + x * 4 + 3] != 255)
                return pixbuf;
    GdkPixbuf* rgb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
    if (!rgb)
        return pixbuf;
    const int out_stride = gdk_pixbuf_get_rowstride(rgb);
    guchar* dst = gdk_pixbuf_get_pixels(rgb);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                dst[y * out_stride + x * 3 + c] = src[y * stride + x * 4 + c];
    g_object_unref(pixbuf);
    return rgb;
}

} // namespace

CoverCache::CoverCache()
    : m_dir(Config::cache_dir() / "covers") {
    std::error_code ec;
    fs::create_directories(m_dir, ec);
    m_dispatcher.connect(sigc::mem_fun(*this, &CoverCache::deliver));
    const unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
    for (unsigned i = 0; i < n; ++i)
        m_threads.emplace_back(&CoverCache::worker, this);
}

CoverCache::~CoverCache() {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
        m_jobs.clear();
    }
    m_wake.notify_all();
    for (auto& t : m_threads)
        t.join();
    for (auto& r : m_results)
        if (r.pixbuf) g_object_unref(r.pixbuf);
}

fs::path CoverCache::path_for(const GameEntry& game) {
    const std::string key = game.path.string();
    if (const auto it = m_paths.find(key); it != m_paths.end())
        return it->second;
    static const auto dirs = cover_dirs();
    return m_paths[key] = find_cover(game, dirs);
}

void CoverCache::forget() {
    m_paths.clear();
}

Glib::RefPtr<Gdk::Texture> CoverCache::lookup(const fs::path& file, int px, bool& exact) {
    exact = false;
    const auto it = m_textures.find(file.string());
    if (it == m_textures.end())
        return {};
    fs::file_time_type time;
    std::uintmax_t size = 0;
    if (!stat_file(file, time, size) || time != it->second.time || size != it->second.size) {
        m_textures.erase(it);           // the cover was replaced
        return {};
    }
    exact = it->second.px == px;
    return it->second.texture;
}

void CoverCache::set_box_art(bool on) {
    if (on == m_box)
        return;
    m_box = on;
    m_textures.clear();
    m_waiting.clear();
    std::lock_guard lock(m_mutex);
    m_jobs.clear();
}

void CoverCache::request(const fs::path& file, int px, Done done) {
    const auto key = std::make_pair(file.string(), px);
    if (px != m_px) {
        // A new size (the cover size slider): earlier sizes are not wanted.
        m_px = px;
        std::lock_guard lock(m_mutex);
        std::erase_if(m_jobs, [px](const Job& j) { return j.px != px; });
        std::erase_if(m_waiting, [px](const auto& w) { return w.first.second != px; });
    }
    auto& waiting = m_waiting[key];
    waiting.push_back(std::move(done));
    if (waiting.size() > 1)
        return;                         // already on its way
    {
        std::lock_guard lock(m_mutex);
        m_jobs.push_back({file, px, m_box});
    }
    m_wake.notify_one();
}

void CoverCache::worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
            if (m_stop)
                return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        GdkPixbuf* pixbuf = make(job);
        {
            std::lock_guard lock(m_mutex);
            if (m_stop) {
                if (pixbuf) g_object_unref(pixbuf);
                return;
            }
            m_results.push_back({std::move(job.file), job.px, job.box, pixbuf});
        }
        m_dispatcher.emit();
    }
}

// Runs on a worker thread: the picture from disk, made first if need be.
GdkPixbuf* CoverCache::make(const Job& job) const {
    if (job.px > kThumbEdge) {
        GdkPixbuf* p = image::load(job.file, job.px, job.px);
        if (p && job.box) {
            GdkPixbuf* box = render_box_art(p, job.px);
            g_object_unref(p);
            return box;
        }
        return p;
    }
    fs::file_time_type time;
    std::uintmax_t size = 0;
    if (!stat_file(job.file, time, size))
        return nullptr;
    char name[40];
    std::snprintf(name, sizeof name, "%016zx",
                  std::hash<std::string>{}(job.file.string() + '\n' +
                                           std::to_string(time.time_since_epoch().count()) + '\n' +
                                           std::to_string(size)));
    if (!job.box)
        return make_flat(job, name);

    // The case is drawn once from the thumbnail and kept beside it. The
    // suffix names the drawing's version, so a changed look redraws.
    const fs::path boxed = m_dir / (std::string(name) + "-box1.png");
    if (GdkPixbuf* p = image::load(boxed))
        return fit(p, job.px);
    Job flat_job = job;
    flat_job.px = kThumbEdge;
    GdkPixbuf* flat = make_flat(flat_job, name);
    if (!flat)
        return nullptr;
    GdkPixbuf* box = render_box_art(flat, kThumbEdge);
    g_object_unref(flat);
    if (!box)
        return nullptr;
    fs::path part = boxed;
    part += ".part";
    std::error_code ec;
    if (image::save_png(box, part))
        fs::rename(part, boxed, ec);
    else
        fs::remove(part, ec);
    return fit(box, job.px);
}

// The flat thumbnail, from disk or made from the original.
GdkPixbuf* CoverCache::make_flat(const Job& job, const std::string& name) const {
    for (const char* ext : {".jpg", ".png"}) {
        const fs::path thumb = m_dir / (name + ext);
        if (GdkPixbuf* p = image::load(thumb))
            return fit(p, job.px);
    }

    // First time for this picture: shrink the original and keep the result.
    int w = 0, h = 0;
    if (!image::info(job.file, w, h))
        return nullptr;
    GdkPixbuf* thumb = image::load(job.file, kThumbEdge, kThumbEdge);
    if (!thumb)
        return nullptr;
    thumb = drop_unused_alpha(thumb);
    // Opaque art compresses far better as JPEG; transparency needs PNG.
    const bool alpha = gdk_pixbuf_get_has_alpha(thumb);
    const fs::path dest = m_dir / (name + (alpha ? ".png" : ".jpg"));
    fs::path part = dest;
    part += ".part";
    const bool saved = alpha ? image::save_png(thumb, part) : image::save_jpeg(thumb, part, 92);
    std::error_code ec;
    if (saved)
        fs::rename(part, dest, ec);     // whole files only, if two threads race
    else
        fs::remove(part, ec);
    return fit(thumb, job.px);
}

void CoverCache::deliver() {
    std::deque<Result> results;
    {
        std::lock_guard lock(m_mutex);
        results.swap(m_results);
    }
    for (auto& r : results) {
        const auto key = std::make_pair(r.file.string(), r.px);
        if (r.box != m_box) {               // made before the style changed
            if (r.pixbuf) g_object_unref(r.pixbuf);
            continue;
        }
        if (!r.pixbuf) {
            m_waiting.erase(key);
            continue;
        }
        auto texture = Gdk::Texture::create_for_pixbuf(Glib::wrap(r.pixbuf, false));
        Entry entry{r.px, {}, 0, texture};
        if (stat_file(r.file, entry.time, entry.size))
            m_textures[r.file.string()] = std::move(entry);
        const auto it = m_waiting.find(key);
        if (it == m_waiting.end())
            continue;
        const auto done = std::move(it->second);
        m_waiting.erase(it);
        for (const auto& d : done)
            d(texture);
    }
}

} // namespace ds::frontend
