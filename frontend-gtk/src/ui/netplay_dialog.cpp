// SPDX-License-Identifier: GPL-3.0-or-later
#include "netplay_dialog.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#include <algorithm>

namespace ds::frontend {

namespace fs = std::filesystem;
using ds::engine::Engine;

namespace {

// This computer's addresses on its networks (IPv4, not loopback).
std::vector<std::string> local_addresses() {
    std::vector<std::string> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0)
        return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || (a->ifa_flags & IFF_LOOPBACK) ||
            !(a->ifa_flags & IFF_UP))
            continue;
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr, text, sizeof text);
        out.emplace_back(text);
    }
    freeifaddrs(list);
    return out;
}

Gtk::Label* caption(const char* text) {
    auto* l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0.f);
    l->add_css_class("heading");
    return l;
}

} // namespace

NetplayDialog::NetplayDialog(Config& config, const GameLibrary& library, const fs::path& preselect)
    : m_config(config) {
    set_title("Online Play");
    set_default_size(520, -1);
    set_modal(true);
    add_css_class("ds-settings");
    m_root.set_margin(18);

    auto* intro = Gtk::make_managed<Gtk::Label>(
        "Play a game with a friend on another computer. One of you hosts, the other joins the "
        "host's address. Pick the same game, from the same files: the game starts when both "
        "are connected.");
    intro->set_wrap(true);
    intro->set_xalign(0.f);
    m_root.append(*intro);

    // The games, by title.
    std::vector<const GameEntry*> games;
    for (const auto& g : library.games())
        games.push_back(&g);
    std::sort(games.begin(), games.end(),
              [](const GameEntry* a, const GameEntry* b) { return a->title < b->title; });
    auto names = Gtk::StringList::create({});
    guint selected = 0;
    for (const GameEntry* g : games) {
        if (g->path == preselect)
            selected = static_cast<guint>(m_paths.size());
        names->append(g->title + "  (" + g->system + ")");
        m_paths.push_back(g->path);
    }
    m_game.set_model(names);
    m_game.set_enable_search(true);
    m_game.set_expression(Gtk::ClosureExpression<Glib::ustring>::create(
        [](const Glib::RefPtr<Glib::ObjectBase>& item) {
            const auto s = std::dynamic_pointer_cast<Gtk::StringObject>(item);
            return s ? s->get_string() : Glib::ustring();
        }));
    if (!m_paths.empty())
        m_game.set_selected(selected);
    m_root.append(*caption("Game"));
    m_root.append(m_game);

    // Host or join.
    auto* mode = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    mode->add_css_class("linked");
    m_join.set_group(m_host);
    m_host.set_hexpand(true);
    m_join.set_hexpand(true);
    mode->append(m_host);
    mode->append(m_join);
    (m_config.network.netplay_host ? m_host : m_join).set_active(true);
    m_host.signal_toggled().connect([this] { update_mode(); });
    m_root.append(*caption("You"));
    m_root.append(*mode);

    m_address.set_placeholder_text("Host's address, e.g. 203.0.113.7 (or 203.0.113.7:19713)");
    m_address.set_text(m_config.network.netplay_peer);
    m_address.signal_activate().connect([this] { start(); });
    m_root.append(m_address);

    auto* delay_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    auto* delay_label = Gtk::make_managed<Gtk::Label>("Input delay (frames)");
    delay_label->set_hexpand(true);
    delay_label->set_xalign(0.f);
    delay_label->set_tooltip_text("Hides lag by delaying everyone's input a little. 0–1 on a "
                                  "local network, 1–3 over the Internet.");
    m_delay.set_range(0, 10);
    m_delay.set_increments(1, 1);
    m_delay.set_value(std::clamp(m_config.network.netplay_delay, 0, 10));
    delay_row->append(*delay_label);
    delay_row->append(m_delay);
    m_root.append(*delay_row);

    m_info.set_wrap(true);
    m_info.set_xalign(0.f);
    m_info.set_selectable(true);
    m_info.add_css_class("dim-label");
    m_root.append(m_info);

    auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    buttons->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    cancel->signal_clicked().connect([this] { close(); });
    m_start.add_css_class("suggested-action");
    m_start.signal_clicked().connect([this] { start(); });
    m_address.signal_changed().connect([this] { update_mode(); });
    buttons->append(*cancel);
    buttons->append(m_start);
    m_root.append(*buttons);

    set_child(m_root);
    update_mode();
}

void NetplayDialog::update_mode() {
    const bool host = m_host.get_active();
    m_address.set_visible(!host);
    m_start.set_sensitive(!m_paths.empty() && (host || !m_address.get_text().empty()));
    const std::string port = std::to_string(Engine::kNetplayPort);
    if (host) {
        std::string addresses;
        for (const auto& a : local_addresses())
            addresses += (addresses.empty() ? "" : ", ") + a;
        m_info.set_text(
            "You are player 1. On the same network your friend joins " +
            (addresses.empty() ? std::string("this computer's address") : addresses) +
            ". Over the Internet they join your public address, and UDP port " + port +
            " must reach this computer: DreamStation asks the router to open it (UPnP); if the "
            "router does not allow that, forward the port to this computer by hand.");
    } else {
        m_info.set_text("You are player 2: your controller 1 plays as player 2. Ask the host "
                        "for their address; the port is " + port + " unless they say otherwise.");
    }
}

void NetplayDialog::start() {
    if (!m_start.get_sensitive())
        return;
    Engine::Netplay netplay;
    netplay.host  = m_host.get_active();
    netplay.peer  = netplay.host ? std::string() : std::string(m_address.get_text());
    netplay.delay = m_delay.get_value_as_int();
    m_config.network.netplay_host  = netplay.host;
    if (!netplay.host)
        m_config.network.netplay_peer = netplay.peer;
    m_config.network.netplay_delay = netplay.delay;
    m_config.save();
    const fs::path game = m_paths[m_game.get_selected()];
    close();
    signal_start.emit(game, netplay);
}

} // namespace ds::frontend
