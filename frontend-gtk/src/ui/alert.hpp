// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gtkmm.h>
#include <adwaita.h>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ds::frontend {

using Responses = std::vector<std::pair<const char*, const char*>>;   // id, label

// The alert on screen, if any (for the scripted test aid's "respond:").
extern AdwDialog* g_open_alert;

// Present an Adwaita alert; `done` receives the chosen response id
// ("cancel" when dismissed).
void alert(Gtk::Window& parent, const std::string& heading, const std::string& body,
           const Responses& responses, const char* default_id, Gtk::Widget* extra,
           std::function<void(const std::string&)> done);

} // namespace ds::frontend
