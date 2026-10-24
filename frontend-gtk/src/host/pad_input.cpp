// SPDX-License-Identifier: GPL-3.0-or-later
#include "pad_input.hpp"
#include <dreamstation/common/log.hpp>
#include <SDL3/SDL.h>
#include <gdk/gdk.h>

#include <algorithm>
#include <cmath>

namespace ds::frontend {

using namespace ds::engine;

// Positional defaults: the bottom face button is the Dreamcast A button
// whatever the pad's own labels say. Source names are SDL's, which identify
// face buttons by position using Xbox letters (a = bottom, b = right,
// x = left, y = top) — the same layout as the Dreamcast pad.
const std::vector<PadInputDef>& pad_input_defs() {
    static const std::vector<PadInputDef> defs = {
        {"a",           "Face Buttons", "A",                "pad:a",              "key:z"},
        {"b",           "Face Buttons", "B",                "pad:b",              "key:x"},
        {"x",           "Face Buttons", "X",                "pad:x",              "key:a"},
        {"y",           "Face Buttons", "Y",                "pad:y",              "key:s"},
        {"start",       "Face Buttons", "Start",            "pad:start",          "key:Return"},
        {"up",          "D-Pad",        "Up",               "pad:dpup",           "key:Up"},
        {"down",        "D-Pad",        "Down",             "pad:dpdown",         "key:Down"},
        {"left",        "D-Pad",        "Left",             "pad:dpleft",         "key:Left"},
        {"right",       "D-Pad",        "Right",            "pad:dpright",        "key:Right"},
        {"stick_up",    "Analog Stick", "Up",               "axis:lefty-",        "key:i"},
        {"stick_down",  "Analog Stick", "Down",             "axis:lefty+",        "key:k"},
        {"stick_left",  "Analog Stick", "Left",             "axis:leftx-",        "key:j"},
        {"stick_right", "Analog Stick", "Right",            "axis:leftx+",        "key:l"},
        {"ltrigger",    "Triggers",     "Left Trigger",     "axis:lefttrigger+",  "key:q"},
        {"rtrigger",    "Triggers",     "Right Trigger",    "axis:righttrigger+", "key:w"},
        {"c",           "Extra (arcade sticks)", "C",       "pad:leftshoulder",   ""},
        {"z",           "Extra (arcade sticks)", "Z",       "pad:rightshoulder",  ""},
        // Coin: each player's own pad (Back / Create / Select), and a key per
        // player so a group can feed one keyboard: C, V, B, N for players 1-4.
        {"d",           "Extra (arcade sticks)", "D / Coin", "pad:back",          "key:c",
         {"key:v", "key:b", "key:n"}},
        // Arcade cabinet switches. No gamepad default: pressing one by
        // accident drops the game into its operator menu.
        {"test",        "Arcade Cabinet", "Test",           "",                   "key:F6"},
        {"service",     "Arcade Cabinet", "Service",        "",                   "key:9"},
        {"rstick_up",   "Second Stick", "Up",               "axis:righty-",       ""},
        {"rstick_down", "Second Stick", "Down",             "axis:righty+",       ""},
        {"rstick_left", "Second Stick", "Left",             "axis:rightx-",       ""},
        {"rstick_right","Second Stick", "Right",            "axis:rightx+",       ""},
    };
    return defs;
}

namespace {

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        const std::size_t end = std::min(s.find('|', pos), s.size());
        if (end > pos)
            out.push_back(s.substr(pos, end - pos));
        pos = end + 1;
    }
    return out;
}

// SDL button for a source name. Also accepts the compass names, which read
// naturally in a hand-edited config.
SDL_GamepadButton button_from_name(const std::string& name) {
    if (name == "south") return SDL_GAMEPAD_BUTTON_SOUTH;
    if (name == "east")  return SDL_GAMEPAD_BUTTON_EAST;
    if (name == "west")  return SDL_GAMEPAD_BUTTON_WEST;
    if (name == "north") return SDL_GAMEPAD_BUTTON_NORTH;
    return SDL_GetGamepadButtonFromString(name.c_str());
}

std::string binding_key(unsigned port, const std::string& id) {
    return std::string(1, static_cast<char>('A' + port)) + "." + id;
}

} // namespace

PadInput& PadInput::instance() {
    static PadInput inst;
    return inst;
}

PadInput::PadInput() {
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i)
            open_pad(ids[i]);
        SDL_free(ids);
    }
    set_bindings({});

    // A default that SDL cannot resolve is a dead button; say so loudly.
    for (const auto& def : pad_input_defs()) {
        const std::string src = def.default_pad;
        if (src.empty())
            continue;
        const bool ok = src.rfind("pad:", 0) == 0
            ? button_from_name(src.substr(4)) != SDL_GAMEPAD_BUTTON_INVALID
            : SDL_GetGamepadAxisFromString(src.substr(5, src.size() - 6).c_str()) !=
                  SDL_GAMEPAD_AXIS_INVALID;
        if (!ok)
            ds::common::log()->error("PadInput: default binding '{}' for '{}' is not a "
                                     "valid gamepad source", src, def.id);
    }
}

PadInput::~PadInput() {
    for (auto& s : m_slots)
        if (s.pad)
            SDL_CloseGamepad(s.pad);
}

void PadInput::open_pad(SDL_JoystickID id) {
    for (const auto& s : m_slots)
        if (s.id == id)
            return;
    for (std::size_t i = 0; i < m_slots.size(); ++i) {
        if (m_slots[i].pad)
            continue;
        SDL_Gamepad* pad = SDL_OpenGamepad(id);
        if (!pad) {
            ds::common::log()->warn("PadInput: cannot open gamepad {}: {}", id, SDL_GetError());
            return;
        }
        m_slots[i] = {id, pad};
        ds::common::log()->info("PadInput: '{}' -> port {}", SDL_GetGamepadName(pad),
                                static_cast<char>('A' + i));
        return;
    }
}

void PadInput::close_pad(SDL_JoystickID id) {
    for (auto& s : m_slots) {
        if (s.id == id && s.pad) {
            SDL_CloseGamepad(s.pad);
            s = {};
        }
    }
}

int PadInput::connected() const {
    int n = 0;
    for (const auto& s : m_slots)
        n += s.pad != nullptr;
    return n;
}

std::string PadInput::device_name(unsigned port) const {
    if (port >= m_slots.size() || !m_slots[port].pad)
        return {};
    const char* name = SDL_GetGamepadName(m_slots[port].pad);
    return name ? name : "Gamepad";
}

// ── Bindings ──────────────────────────────────────────────────────────────

std::string PadInput::default_binding(unsigned port, const PadInputDef& def) {
    std::string b = def.default_pad;
    const char* key = port == 0 ? def.default_key : port < 4 ? def.other_ports_keys[port - 1] : nullptr;
    if (key && key[0])
        b += (b.empty() ? "" : "|") + std::string(key);
    return b;
}

std::string PadInput::binding(unsigned port, const std::string& input_id) const {
    const auto it = m_bindings.find(binding_key(port, input_id));
    if (it != m_bindings.end())
        return it->second;
    for (const auto& def : pad_input_defs())
        if (input_id == def.id)
            return default_binding(port, def);
    return {};
}

void PadInput::set_bindings(const std::map<std::string, std::string>& bindings) {
    m_bindings = bindings;
    m_bound_keys.clear();
    for (unsigned port = 0; port < 4; ++port)
        for (const auto& def : pad_input_defs())
            for (const auto& src : split(binding(port, def.id)))
                if (src.rfind("key:", 0) == 0) {
                    const guint kv = gdk_keyval_from_name(src.c_str() + 4);
                    if (kv != GDK_KEY_VoidSymbol && kv != 0)
                        m_bound_keys.insert(gdk_keyval_to_lower(kv));
                }
}

std::string PadInput::describe(const std::string& binding, unsigned port) const {
    if (binding.empty())
        return "—";
    SDL_Gamepad* pad = port < m_slots.size() ? m_slots[port].pad : nullptr;
    const auto face_label = [pad](const std::string& name) -> std::string {
        const SDL_GamepadButton b = button_from_name(name);
        if (!pad || b == SDL_GAMEPAD_BUTTON_INVALID)
            return name;
        switch (SDL_GetGamepadButtonLabel(pad, b)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A:        return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B:        return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X:        return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y:        return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS:    return "Cross";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:   return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:   return "Square";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
        default:                                return name;
        }
    };
    std::string out;
    for (const auto& src : split(binding)) {
        if (!out.empty()) out += "  /  ";
        if (src.rfind("pad:", 0) == 0)       out += "Pad " + face_label(src.substr(4));
        else if (src.rfind("axis:", 0) == 0) out += "Axis " + src.substr(5);
        else if (src.rfind("key:", 0) == 0)  out += "Key " + src.substr(4);
        else                                 out += src;
    }
    return out;
}

bool PadInput::set_key(unsigned keyval, bool pressed) {
    const unsigned kv = gdk_keyval_to_lower(keyval);
    if (pressed) m_keys.insert(kv);
    else         m_keys.erase(kv);
    return m_bound_keys.count(kv) != 0;
}

// Value of one source in 0..1.
float PadInput::source_value(unsigned port, const std::string& source) const {
    if (source.rfind("key:", 0) == 0) {
        const guint kv = gdk_keyval_from_name(source.c_str() + 4);
        return m_keys.count(gdk_keyval_to_lower(kv)) ? 1.f : 0.f;
    }
    SDL_Gamepad* pad = m_slots[port].pad;
    if (!pad)
        return 0.f;
    if (source.rfind("pad:", 0) == 0) {
        const SDL_GamepadButton b = button_from_name(source.substr(4));
        return (b != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(pad, b)) ? 1.f : 0.f;
    }
    if (source.rfind("axis:", 0) == 0 && source.size() > 6) {
        const char dir = source.back();
        const std::string name = source.substr(5, source.size() - 6);
        const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(name.c_str());
        if (a == SDL_GAMEPAD_AXIS_INVALID)
            return 0.f;
        const float v = static_cast<float>(SDL_GetGamepadAxis(pad, a)) / 32767.f;
        return std::clamp(dir == '-' ? -v : v, 0.f, 1.f);
    }
    return 0.f;
}

float PadInput::input_value(unsigned port, const std::string& input_id) const {
    float best = 0.f;
    for (const auto& src : split(binding(port, input_id)))
        best = std::max(best, source_value(port, src));
    return best;
}

// ── Frame update ──────────────────────────────────────────────────────────

void PadInput::pump() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_GAMEPAD_ADDED)
            open_pad(ev.gdevice.which);
        else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED)
            close_pad(ev.gdevice.which);
    }
}

void PadInput::update() {
    pump();

    static const struct { const char* id; PadButton button; } kDigital[] = {
        {"a", PAD_B}, {"b", PAD_A}, {"x", PAD_Y}, {"y", PAD_X}, {"start", PAD_START},
        {"up", PAD_UP}, {"down", PAD_DOWN}, {"left", PAD_LEFT}, {"right", PAD_RIGHT},
        {"c", PAD_L}, {"z", PAD_R}, {"d", PAD_SELECT},
        {"test", PAD_L3}, {"service", PAD_R3},
    };
    const auto axis = [](float pos, float neg) {
        return static_cast<std::int16_t>(std::lround(std::clamp(pos - neg, -1.f, 1.f) * 32767.f));
    };

    Engine& engine = Engine::instance();
    for (unsigned port = 0; port < m_slots.size(); ++port) {
        PadState st{};
        for (const auto& d : kDigital)
            if (input_value(port, d.id) > 0.5f)
                st.buttons |= static_cast<std::uint16_t>(1u << d.button);
        st.lx = axis(input_value(port, "stick_right"),  input_value(port, "stick_left"));
        st.ly = axis(input_value(port, "stick_down"),   input_value(port, "stick_up"));
        st.rx = axis(input_value(port, "rstick_right"), input_value(port, "rstick_left"));
        st.ry = axis(input_value(port, "rstick_down"),  input_value(port, "rstick_up"));
        st.l2 = static_cast<std::int16_t>(std::lround(input_value(port, "ltrigger") * 32767.f));
        st.r2 = static_cast<std::int16_t>(std::lround(input_value(port, "rtrigger") * 32767.f));
        if (st.l2 > 16000) st.buttons |= 1u << PAD_L2;
        if (st.r2 > 16000) st.buttons |= 1u << PAD_R2;
        engine.pad(port) = st;
    }
}

unsigned PadInput::nav_state() {
    pump();
    SDL_UpdateGamepads();
    SDL_Gamepad* pad = m_slots[0].pad;
    if (!pad)
        return 0;
    // Fixed layout for menus, independent of the game bindings.
    const auto down = [pad](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b); };
    const int lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    const int ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    unsigned state = 0;
    if (down(SDL_GAMEPAD_BUTTON_DPAD_UP)    || ly < -20000) state |= NavUp;
    if (down(SDL_GAMEPAD_BUTTON_DPAD_DOWN)  || ly >  20000) state |= NavDown;
    if (down(SDL_GAMEPAD_BUTTON_DPAD_LEFT)  || lx < -20000) state |= NavLeft;
    if (down(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx >  20000) state |= NavRight;
    if (down(SDL_GAMEPAD_BUTTON_SOUTH)) state |= NavAccept;
    if (down(SDL_GAMEPAD_BUTTON_EAST))  state |= NavBack;
    if (down(SDL_GAMEPAD_BUTTON_START)) state |= NavStart;
    return state;
}

unsigned PadInput::held_buttons() {
    pump();
    SDL_UpdateGamepads();
    SDL_Gamepad* pad = m_slots[0].pad;
    unsigned mask = 0;
    if (pad)
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; ++b)
            if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b)))
                mask |= 1u << b;
    return mask;
}

void PadInput::rumble(unsigned port, std::uint16_t strong, std::uint16_t weak) {
    if (port >= m_slots.size() || !m_slots[port].pad)
        return;
    if (!m_rumble_enabled)
        strong = weak = 0;
    // The engine reports changes, not durations: hold the level until the
    // next report, with a cap so a missed "stop" cannot buzz forever.
    SDL_RumbleGamepad(m_slots[port].pad, strong, weak, (strong || weak) ? 2000 : 0);
}

// ── Capture ───────────────────────────────────────────────────────────────

void PadInput::begin_capture(unsigned port) {
    pump();
    m_capture_port = std::min<unsigned>(port, 3);
    SDL_Gamepad* pad = m_slots[m_capture_port].pad;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        m_capture_buttons[static_cast<std::size_t>(b)] =
            pad && SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b));
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
        m_capture_axes[static_cast<std::size_t>(a)] =
            pad ? SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(a)) : 0;
}

std::optional<std::string> PadInput::poll_capture() {
    pump();
    SDL_UpdateGamepads();
    SDL_Gamepad* pad = m_slots[m_capture_port].pad;
    if (!pad)
        return std::nullopt;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
        const auto button = static_cast<SDL_GamepadButton>(b);
        const bool down = SDL_GetGamepadButton(pad, button);
        if (down && !m_capture_buttons[static_cast<std::size_t>(b)])
            if (const char* name = SDL_GetGamepadStringForButton(button))
                return std::string("pad:") + name;
        m_capture_buttons[static_cast<std::size_t>(b)] = down;
    }
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
        const auto ax = static_cast<SDL_GamepadAxis>(a);
        const int now = SDL_GetGamepadAxis(pad, ax);
        const int was = m_capture_axes[static_cast<std::size_t>(a)];
        // A decisive move away from where the axis rested when capture began.
        if (std::abs(now - was) > 20000)
            if (const char* name = SDL_GetGamepadStringForAxis(ax))
                return std::string("axis:") + name + (now > was ? "+" : "-");
    }
    return std::nullopt;
}

} // namespace ds::frontend
