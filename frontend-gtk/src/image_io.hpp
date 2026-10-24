// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdkmm/pixbuf.h>
#include <filesystem>

namespace ds::frontend::image {

// Reading and writing picture files without gdk-pixbuf's loaders, which some
// distributions hand to external helper programs (glycin) that a bundled app
// may not find. Pixels still travel as GdkPixbufs.
// Reads PNG, JPEG, BMP, TGA, GIF (first frame), PSD, HDR, PIC and PNM.

// The picture, shrunk to fit max_w x max_h keeping its shape when those are
// set (never enlarged). Null if it cannot be read. Safe on any thread.
GdkPixbuf* load(const std::filesystem::path& file, int max_w = 0, int max_h = 0);
Glib::RefPtr<Gdk::Pixbuf> load_ref(const std::filesystem::path& file, int max_w = 0, int max_h = 0);

// Size without decoding; false if not a picture this can read.
bool info(const std::filesystem::path& file, int& w, int& h);

bool save_png(GdkPixbuf* pixbuf, const std::filesystem::path& file);
bool save_png(const Glib::RefPtr<Gdk::Pixbuf>& pixbuf, const std::filesystem::path& file);
bool save_jpeg(GdkPixbuf* pixbuf, const std::filesystem::path& file, int quality);

} // namespace ds::frontend::image
