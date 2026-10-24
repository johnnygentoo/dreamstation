// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gdk-pixbuf/gdk-pixbuf.h>

namespace ds::frontend {

// A cover drawn as a game case standing at an angle: the cover on the front,
// the spine turned towards the viewer (the cover's left edge wrapping round),
// a sheen on the plastic and a soft shadow, on a transparent background.
// The result fits `edge` x `edge` pixels. Does not take `cover`; returns a
// new pixbuf (null on failure). Plain CPU work, safe on any thread.
GdkPixbuf* render_box_art(GdkPixbuf* cover, int edge);

} // namespace ds::frontend
