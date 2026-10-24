// SPDX-License-Identifier: GPL-3.0-or-later
#include "osd_layer.hpp"
#include "../image_io.hpp"

#include <algorithm>

namespace ds::frontend {

OsdLayer::OsdLayer() : Gtk::Box(Gtk::Orientation::VERTICAL, 0) {
    add_css_class("ds-osd");
    set_hexpand(true);
    set_vexpand(true);
    set_can_target(false);          // input passes through to the game
    set_can_focus(false);
    set_margin(14);

    m_messages.add_css_class("ds-osd-messages");
    m_perf.add_css_class("ds-osd-panel");
    m_perf.set_visible(false);
    m_perf.set_halign(Gtk::Align::END);
    m_perf.set_valign(Gtk::Align::START);

    m_slot.add_css_class("ds-osd-panel");
    m_slot.set_visible(false);
    m_slot.set_halign(Gtk::Align::CENTER);
    m_slot_picture.set_halign(Gtk::Align::CENTER);
    m_slot_picture.set_can_shrink(false);
    m_slot_title.add_css_class("ds-osd-title");
    m_slot_legend.add_css_class("dim-label");
    m_slot.append(m_slot_picture);
    m_slot.append(m_slot_title);
    m_slot.append(m_slot_detail);
    m_slot.append(m_slot_legend);

    // The VMU screen: its dots at 3x, in the VMU's own colours.
    m_vmu.set_visible(false);
    m_vmu.set_content_width(48 * 3 + 12);
    m_vmu.set_content_height(32 * 3 + 12);
    m_vmu.set_valign(Gtk::Align::END);
    m_vmu.set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
        cr->set_source_rgba(0.62, 0.70, 0.63, 0.9);
        cr->rectangle(0, 0, w, h);
        cr->fill();
        const double scale = std::min((w - 12) / 48.0, (h - 12) / 32.0);
        const double ox = (w - 48 * scale) / 2, oy = (h - 32 * scale) / 2;
        cr->set_source_rgb(0.09, 0.13, 0.11);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 48; ++x)
                if (m_vmu_dots[static_cast<std::size_t>(y * 48 + x)])
                    cr->rectangle(ox + x * scale, oy + y * scale, scale, scale);
        cr->fill();
    });

    // Three rows: top bar, flexible middle, bottom bar.
    m_top.set_hexpand(true);
    m_bottom.set_hexpand(true);
    auto* middle = Gtk::make_managed<Gtk::Box>();
    middle->set_vexpand(true);
    append(m_top);
    append(*middle);
    append(m_bottom);
    set_message_corner(m_corner);

    m_scale_css = Gtk::CssProvider::create();
    Gtk::StyleProvider::add_provider_for_display(Gdk::Display::get_default(), m_scale_css,
                                                 GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
}

OsdLayer::~OsdLayer() { m_timer.disconnect(); }

void OsdLayer::set_scale(int percent) {
    percent = std::clamp(percent, 50, 400);
    m_scale_css->load_from_data(".ds-osd label { font-size: " + std::to_string(percent) + "%; }");
}

// Messages sit in one corner; the performance panel takes the top right
// unless the messages are there, and the slot selector the bottom centre.
void OsdLayer::set_message_corner(const std::string& corner) {
    m_corner = corner;
    for (Gtk::Widget* w : {static_cast<Gtk::Widget*>(&m_messages),
                           static_cast<Gtk::Widget*>(&m_perf),
                           static_cast<Gtk::Widget*>(&m_slot),
                           static_cast<Gtk::Widget*>(&m_vmu)})
        if (w->get_parent())
            w->unparent();
    m_top.unset_start_widget();    m_top.unset_center_widget();    m_top.unset_end_widget();
    m_bottom.unset_start_widget(); m_bottom.unset_center_widget(); m_bottom.unset_end_widget();

    const bool bottom = corner.rfind("bottom", 0) == 0;
    Gtk::CenterBox& bar = bottom ? m_bottom : m_top;
    m_messages.set_valign(bottom ? Gtk::Align::END : Gtk::Align::START);
    if (corner.find("center") != std::string::npos) {
        m_messages.set_halign(Gtk::Align::CENTER);
        bar.set_center_widget(m_messages);
    } else if (corner.find("right") != std::string::npos) {
        m_messages.set_halign(Gtk::Align::END);
        bar.set_end_widget(m_messages);
    } else {
        m_messages.set_halign(Gtk::Align::START);
        bar.set_start_widget(m_messages);
    }
    if (corner == "top-right") m_top.set_start_widget(m_perf);
    else                       m_top.set_end_widget(m_perf);
    if (corner == "bottom-center") m_top.set_center_widget(m_slot);
    else                           m_bottom.set_center_widget(m_slot);
    m_perf.set_halign(corner == "top-right" ? Gtk::Align::START : Gtk::Align::END);
    // The VMU screen takes the bottom right, or the bottom left if the
    // messages are there.
    if (corner == "bottom-right") {
        m_vmu.set_halign(Gtk::Align::START);
        m_bottom.set_start_widget(m_vmu);
    } else {
        m_vmu.set_halign(Gtk::Align::END);
        m_bottom.set_end_widget(m_vmu);
    }
}

void OsdLayer::set_vmu_screen(const std::uint8_t* dots) {
    m_vmu.set_visible(dots != nullptr);
    if (!dots)
        return;
    std::copy(dots, dots + m_vmu_dots.size(), m_vmu_dots.begin());
    m_vmu.queue_draw();
}

void OsdLayer::ensure_timer() {
    if (!m_timer.connected())
        m_timer = Glib::signal_timeout().connect(sigc::mem_fun(*this, &OsdLayer::tick), 100);
}

void OsdLayer::message(const std::string& text, double seconds) {
    const gint64 expires = g_get_monotonic_time() + static_cast<gint64>(seconds * 1e6);
    if (!m_active.empty() && m_active.back().label->get_text() == text) {
        m_active.back().expires_us = expires;
        return;
    }
    auto* label = Gtk::make_managed<Gtk::Label>(text);
    label->add_css_class("ds-osd-message");
    label->set_xalign(0.f);
    label->set_wrap(true);
    label->set_max_width_chars(60);
    m_messages.append(*label);
    m_active.push_back({label, expires});
    while (m_active.size() > 6) {              // keep the stack short
        m_messages.remove(*m_active.front().label);
        m_active.pop_front();
    }
    ensure_timer();
}

void OsdLayer::clear_messages() {
    for (auto& m : m_active)
        m_messages.remove(*m.label);
    m_active.clear();
}

void OsdLayer::set_performance(const std::vector<std::string>& lines) {
    while (auto* child = m_perf.get_first_child())
        m_perf.remove(*child);
    for (const auto& line : lines) {
        auto* label = Gtk::make_managed<Gtk::Label>(line);
        label->set_xalign(1.f);
        m_perf.append(*label);
    }
    m_perf.set_visible(!lines.empty());
}

void OsdLayer::show_slot(const SlotInfo& info) {
    m_slot_title.set_text(info.title);
    m_slot_detail.set_text(info.detail);
    m_slot_legend.set_text(info.legend);
    if (!info.screenshot.empty() && std::filesystem::exists(info.screenshot)) {
        // Decoded at thumbnail size so the panel stays small.
        if (const auto pixbuf = image::load_ref(info.screenshot, 256, 192)) {
            m_slot_picture.set_paintable(Gdk::Texture::create_for_pixbuf(pixbuf));
            m_slot_picture.set_visible(true);
        } else {
            m_slot_picture.set_visible(false);
        }
    } else {
        m_slot_picture.set_visible(false);
    }
    m_slot.set_visible(true);
    m_slot_expires_us = g_get_monotonic_time() + 2500000;
    ensure_timer();
}

bool OsdLayer::tick() {
    const gint64 now = g_get_monotonic_time();
    while (!m_active.empty() && m_active.front().expires_us <= now) {
        m_messages.remove(*m_active.front().label);
        m_active.pop_front();
    }
    // Messages added later may expire sooner; sweep the rest too.
    for (auto it = m_active.begin(); it != m_active.end();) {
        if (it->expires_us <= now) {
            m_messages.remove(*it->label);
            it = m_active.erase(it);
        } else {
            ++it;
        }
    }
    if (m_slot.get_visible() && m_slot_expires_us <= now)
        m_slot.set_visible(false);
    return !m_active.empty() || m_slot.get_visible();
}

} // namespace ds::frontend
