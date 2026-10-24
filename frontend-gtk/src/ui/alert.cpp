// SPDX-License-Identifier: GPL-3.0-or-later
#include "alert.hpp"

namespace ds::frontend {

AdwDialog* g_open_alert = nullptr;

void alert(Gtk::Window& parent, const std::string& heading, const std::string& body,
           const Responses& responses, const char* default_id, Gtk::Widget* extra,
           std::function<void(const std::string&)> done) {
    AdwDialog* dialog = adw_alert_dialog_new(heading.c_str(), body.c_str());
    auto* alert_dialog = ADW_ALERT_DIALOG(dialog);
    for (const auto& [id, label] : responses)
        adw_alert_dialog_add_response(alert_dialog, id, label);
    adw_alert_dialog_set_default_response(alert_dialog, default_id);
    adw_alert_dialog_set_close_response(alert_dialog, "cancel");
    adw_alert_dialog_set_response_appearance(alert_dialog, default_id, ADW_RESPONSE_SUGGESTED);
    if (extra)
        adw_alert_dialog_set_extra_child(alert_dialog, extra->gobj());

    auto* callback = new std::function<void(const std::string&)>(std::move(done));
    g_signal_connect(dialog, "response",
        G_CALLBACK(+[](AdwAlertDialog* self, const char* response, gpointer data) {
            auto* fn = static_cast<std::function<void(const std::string&)>*>(data);
            // Exactly once: closing the dialog can emit "response" again.
            g_signal_handlers_disconnect_by_data(self, data);
            g_open_alert = nullptr;
            (*fn)(response ? response : "cancel");
            delete fn;
        }), callback);
    g_open_alert = dialog;
    adw_dialog_present(dialog, GTK_WIDGET(parent.gobj()));
}

} // namespace ds::frontend
