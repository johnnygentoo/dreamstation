// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <dreamstation/engine/engine.hpp>
#include <SDL3/SDL_gamepad.h>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ds::frontend {

// One bindable input of a Dreamcast controller port.
struct PadInputDef {
    const char* id;          // config key suffix, e.g. "a", "stick_up"
    const char* group;       // heading in the bindings page
    const char* label;
    const char* default_pad; // default gamepad source ("pad:south", "axis:leftx-")
    const char* default_key; // default keyboard source for port A ("key:z"), or ""
    // Keyboard defaults for ports B, C and D, for inputs every player needs
    // without a gamepad (arcade coins). Usually none.
    const char* other_ports_keys[3] = {nullptr, nullptr, nullptr};
};

const std::vector<PadInputDef>& pad_input_defs();

// Maps SDL3 gamepads and the keyboard onto the engine's four controller
// ports through user-editable bindings. Main-thread only.
//
// A binding is a list of sources separated by '|':
//   pad:<button>      SDL gamepad button name (south, dpup, leftshoulder, ...)
//   axis:<axis>+ / -  SDL gamepad axis name and direction (leftx-, lefttrigger+)
//   key:<name>        GDK key name (z, Return, Up, ...)
// Bindings live in Config::bindings under "<port>.<input id>", port = A..D.
// A missing entry means "use the default"; an empty one means unbound.
class PadInput {
public:
    static PadInput& instance();

    void set_bindings(const std::map<std::string, std::string>& bindings);

    // Pump SDL, pick up hot-plugs and write every port's state to the engine.
    // Always a full re-read, so a dropped Bluetooth report cannot stick.
    void update();

    // Keyboard state, fed from the window. Returns true if some binding
    // uses this key (so the window can consume the event).
    bool set_key(unsigned keyval, bool pressed);
    void clear_keys() { m_keys.clear(); }

    int         connected() const;
    std::string device_name(unsigned port) const;   // "" when no pad on that port

    // Effective binding of an input ("" = unbound) and its default.
    std::string binding(unsigned port, const std::string& input_id) const;
    static std::string default_binding(unsigned port, const PadInputDef& def);
    // Human-readable form of a binding string. Face buttons are named as on
    // the pad plugged into `port` (Cross, Circle, ... on a PlayStation pad).
    std::string describe(const std::string& binding, unsigned port) const;

    // Menu navigation from the controller on port A (works while the game
    // is paused, when the engine is not polling). Bitmask of Nav* values.
    enum Nav : unsigned { NavUp = 1, NavDown = 2, NavLeft = 4, NavRight = 8,
                          NavAccept = 16, NavBack = 32, NavStart = 64 };
    unsigned nav_state();

    // Buttons held on port A's pad, one bit per SDL gamepad button index
    // (for gamepad hotkeys). Safe to call while the game is paused.
    unsigned held_buttons();
    // Drive a pad's rumble motors; strengths are 0..65535, 0/0 stops.
    void rumble(unsigned port, std::uint16_t strong, std::uint16_t weak);
    void set_rumble_enabled(bool on) { m_rumble_enabled = on; }

    // Binding capture for the controllers window: begin, then poll until a
    // gamepad source ("pad:..." / "axis:...") is produced on that port.
    void begin_capture(unsigned port);
    std::optional<std::string> poll_capture();

private:
    PadInput();
    ~PadInput();
    void pump();
    void open_pad(SDL_JoystickID id);
    void close_pad(SDL_JoystickID id);
    float source_value(unsigned port, const std::string& source) const;
    float input_value(unsigned port, const std::string& input_id) const;

    struct Slot {
        SDL_JoystickID id  = 0;
        SDL_Gamepad*   pad = nullptr;
    };
    std::array<Slot, 4>                m_slots{};
    std::set<unsigned>                 m_keys;       // lower-cased GDK keyvals held
    std::map<std::string, std::string> m_bindings;   // overrides from the config
    std::set<unsigned>                 m_bound_keys; // keyvals used by any binding

    bool                           m_rumble_enabled = true;
    unsigned                       m_capture_port = 0;
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> m_capture_buttons{};
    std::array<int, SDL_GAMEPAD_AXIS_COUNT>    m_capture_axes{};
};

} // namespace ds::frontend
