// SPDX-License-Identifier: GPL-3.0-or-later
#include "hotkeys.hpp"
#include <SDL3/SDL_gamepad.h>

namespace ds::frontend {

const std::vector<HotkeyDef>& hotkey_defs() {
    static const std::vector<HotkeyDef> defs = {
        // Interface
        {"pause-menu",        "Interface",   "Open Pause Menu",            "Escape", false, "guide"},
        {"screenshot",        "Interface",   "Save Screenshot",            "F10",    false},
        {"toggle-pause",      "Interface",   "Toggle Pause",               "space",  false},
        {"toggle-fullscreen", "Interface",   "Toggle Fullscreen",          "F11",    false},
        // System
        {"fast-forward",      "System",      "Fast Forward (Hold)",        "Tab",    true},
        {"toggle-fast-forward","System",     "Fast Forward (Toggle)",      "",       false},
        {"frame-step",        "System",      "Frame Step",                 "",       false},
        {"speed-up",          "System",      "Increase Emulation Speed",   "",       false},
        {"speed-down",        "System",      "Decrease Emulation Speed",   "",       false},
        {"speed-reset",       "System",      "Reset Emulation Speed",      "",       false},
        {"close-game",        "System",      "Close Game",                 "",       false},
        {"restart-game",      "System",      "Restart Game",               "",       false},
        // Graphics
        {"toggle-shader",     "Graphics",    "Toggle Post-Processing",     "",       false},
        {"toggle-widescreen", "Graphics",    "Toggle Widescreen",          "",       false},
        {"resolution-up",     "Graphics",    "Increase Resolution Scale",  "",       false},
        {"resolution-down",   "Graphics",    "Decrease Resolution Scale",  "",       false},
        // Audio
        {"toggle-mute",       "Audio",       "Toggle Mute",                "",       false},
        {"volume-up",         "Audio",       "Volume Up",                  "",       false},
        {"volume-down",       "Audio",       "Volume Down",                "",       false},
        // Save states
        {"load-selected",     "Save States", "Load From Selected Slot",    "F1",     false},
        {"save-selected",     "Save States", "Save To Selected Slot",      "F2",     false},
        {"prev-slot",         "Save States", "Select Previous Save Slot",  "F3",     false},
        {"next-slot",         "Save States", "Select Next Save Slot",      "F4",     false},
        {"undo-load-state",   "Save States", "Undo Load State",            "",       false},
    };
    return defs;
}

void HotkeyMap::rebuild(const std::map<std::string, std::string>& overrides,
                        const std::map<std::string, std::string>& pad_overrides) {
    m_by_key.clear();
    m_key_by_id.clear();
    m_by_pad.clear();
    m_pad_by_id.clear();
    for (const HotkeyDef& def : hotkey_defs()) {
        std::string pad = def.default_pad;
        if (auto it = pad_overrides.find(def.id); it != pad_overrides.end())
            pad = it->second;
        m_pad_by_id[def.id] = pad;
        if (pad.empty())
            continue;
        const int button = SDL_GetGamepadButtonFromString(pad.c_str());
        if (button >= 0)
            m_by_pad[button] = &def;
    }
    for (const HotkeyDef& def : hotkey_defs()) {
        std::string key = def.default_key;
        if (auto it = overrides.find(def.id); it != overrides.end())
            key = it->second;
        m_key_by_id[def.id] = key;
        if (key.empty())
            continue;
        const guint keyval = gdk_keyval_from_name(key.c_str());
        if (keyval != GDK_KEY_VoidSymbol && keyval != 0)
            m_by_key[gdk_keyval_to_lower(keyval)] = &def;
    }
}

const HotkeyDef* HotkeyMap::lookup(guint keyval) const {
    const auto it = m_by_key.find(gdk_keyval_to_lower(keyval));
    return it == m_by_key.end() ? nullptr : it->second;
}

const HotkeyDef* HotkeyMap::lookup_pad(int button) const {
    const auto it = m_by_pad.find(button);
    return it == m_by_pad.end() ? nullptr : it->second;
}

std::string HotkeyMap::pad_for(const std::string& id) const {
    const auto it = m_pad_by_id.find(id);
    return it == m_pad_by_id.end() ? std::string{} : it->second;
}

std::string HotkeyMap::key_for(const std::string& id) const {
    const auto it = m_key_by_id.find(id);
    return it == m_key_by_id.end() ? std::string{} : it->second;
}

} // namespace ds::frontend
