// SPDX-License-Identifier: GPL-3.0-or-later
#include "image_io.hpp"

#include <algorithm>
#include <vector>

// Private copies of stb (static functions), so they cannot clash with the
// engine's own.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace ds::frontend::image {

namespace fs = std::filesystem;

GdkPixbuf* load(const fs::path& file, int max_w, int max_h) {
    int w = 0, h = 0, channels = 0;
    stbi_uc* data = stbi_load(file.c_str(), &w, &h, &channels, 4);
    if (!data)
        return nullptr;
    GdkPixbuf* pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w, h);
    if (!pixbuf) {
        stbi_image_free(data);
        return nullptr;
    }
    const int stride = gdk_pixbuf_get_rowstride(pixbuf);
    guchar* dst = gdk_pixbuf_get_pixels(pixbuf);
    for (int y = 0; y < h; ++y)
        std::copy_n(data + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4,
                    static_cast<std::size_t>(w) * 4, dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride));
    stbi_image_free(data);

    if ((max_w > 0 && w > max_w) || (max_h > 0 && h > max_h)) {
        const double sx = max_w > 0 ? static_cast<double>(max_w) / w : 1e9;
        const double sy = max_h > 0 ? static_cast<double>(max_h) / h : 1e9;
        const double scale = std::min(sx, sy);
        const int nw = std::max(1, static_cast<int>(w * scale + 0.5));
        const int nh = std::max(1, static_cast<int>(h * scale + 0.5));
        GdkPixbuf* small = gdk_pixbuf_scale_simple(pixbuf, nw, nh, GDK_INTERP_BILINEAR);
        g_object_unref(pixbuf);
        pixbuf = small;
    }
    return pixbuf;
}

Glib::RefPtr<Gdk::Pixbuf> load_ref(const fs::path& file, int max_w, int max_h) {
    GdkPixbuf* p = load(file, max_w, max_h);
    return p ? Glib::wrap(p, false) : Glib::RefPtr<Gdk::Pixbuf>();
}

bool info(const fs::path& file, int& w, int& h) {
    int channels = 0;
    return stbi_info(file.c_str(), &w, &h, &channels) != 0;
}

namespace {

// The pixbuf's pixels as tightly packed rows of `channels` bytes.
std::vector<unsigned char> packed(GdkPixbuf* p, int& channels) {
    const int w = gdk_pixbuf_get_width(p), h = gdk_pixbuf_get_height(p);
    channels = gdk_pixbuf_get_n_channels(p);
    const int stride = gdk_pixbuf_get_rowstride(p);
    const guchar* src = gdk_pixbuf_read_pixels(p);
    std::vector<unsigned char> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * static_cast<std::size_t>(channels));
    for (int y = 0; y < h; ++y)
        std::copy_n(src + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride),
                    static_cast<std::size_t>(w) * static_cast<std::size_t>(channels),
                    out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * static_cast<std::size_t>(channels));
    return out;
}

} // namespace

bool save_png(GdkPixbuf* p, const fs::path& file) {
    if (!p || gdk_pixbuf_get_bits_per_sample(p) != 8)
        return false;
    int channels = 0;
    const auto data = packed(p, channels);
    const int w = gdk_pixbuf_get_width(p);
    return stbi_write_png(file.c_str(), w, gdk_pixbuf_get_height(p), channels, data.data(), w * channels) != 0;
}

bool save_png(const Glib::RefPtr<Gdk::Pixbuf>& p, const fs::path& file) {
    return p && save_png(p->gobj(), file);
}

bool save_jpeg(GdkPixbuf* p, const fs::path& file, int quality) {
    if (!p || gdk_pixbuf_get_bits_per_sample(p) != 8)
        return false;
    int channels = 0;
    const auto data = packed(p, channels);
    return stbi_write_jpg(file.c_str(), gdk_pixbuf_get_width(p), gdk_pixbuf_get_height(p), channels,
                          data.data(), quality) != 0;
}

} // namespace ds::frontend::image
