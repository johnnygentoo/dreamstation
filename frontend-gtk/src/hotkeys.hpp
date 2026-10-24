// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gdk/gdk.h>
#include <map>
#include <string>
#include <vector>

namespace ds::frontend {

// One bindable action. Defaults follow DuckStation's (Space, Tab, F1-F4,
// F10, F11, Escape); everything else starts unbound.
struct HotkeyDef {
    const char* id;
    const char* category;
    const char* label;
    const char* default_key;   // GDK key name, "" = unbound
    bool        hold;          // true: active while held (fast forward)
    const char* default_pad = "";  // SDL gamepad button name, "" = unbound
};

const std::vector<HotkeyDef>& hotkey_defs();

// Resolves key presses to action ids using the user's overrides
// (Config::hotkeys: action id -> GDK key name, "" to unbind).
class HotkeyMap {
public:
    void rebuild(const std::map<std::string, std::string>& overrides,
                 const std::map<std::string, std::string>& pad_overrides = {});
    // Hotkey bound to a gamepad button (SDL button index), or nullptr.
    const HotkeyDef* lookup_pad(int button) const;
    std::string pad_for(const std::string& id) const;
    // nullptr when the key is not bound.
    const HotkeyDef* lookup(guint keyval) const;
    // Key name currently bound to an action ("" if none).
    std::string key_for(const std::string& id) const;

private:
    std::map<guint, const HotkeyDef*> m_by_key;
    std::map<std::string, std::string> m_key_by_id;
    std::map<int, const HotkeyDef*>    m_by_pad;
    std::map<std::string, std::string> m_pad_by_id;
};

} // namespace ds::frontend
