// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <dreamstation/common/types.hpp>
#include "post_types.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ds::frontend {

struct Config {
    struct Paths {
        std::filesystem::path              bios;
        std::filesystem::path              flash;
        std::vector<std::filesystem::path> game_dirs;
        std::vector<std::filesystem::path> excluded;   // hidden from the game list
    };

    struct Graphics {
        u32  internal_scale = 0;   // 0 = automatic (match the window), else N x 640x480
        bool vsync          = true;
    };

    struct Interface {
        bool        custom_theme    = true;
        bool        show_toolbar    = true;
        bool        show_status_bar = true;
        std::string view_mode       = "library"; // "library" | "game"
        bool        vmu_window      = false;     // the VMU window is open
    };

    // Window/emulation behaviour (DuckStation's Interface page).
    struct Behavior {
        bool confirm_close            = true;
        bool save_state_on_close      = true;
        bool pause_on_focus_loss      = false;
        bool pause_in_library         = true;
        bool hide_cursor_fullscreen   = true;
        bool inhibit_screensaver      = true;
        bool double_click_fullscreen  = true;
        bool start_fullscreen         = false;
        bool rumble                   = true;
    };

    // On-screen display over the game.
    struct Osd {
        bool        messages    = true;
        std::string position    = "top-left";
        int         scale       = 100;     // percent
        bool        show_fps    = true;
        bool        show_speed  = false;
        bool        show_resolution = false;
        bool        show_frame_times = false;
        bool        vmu_screen  = false;   // VMU A1's screen over the game
    };

    // How the finished image is fitted to the window.
    struct Display {
        std::string aspect  = "auto";      // auto | 4:3 | 16:9 | stretch
        std::string filter  = "bilinear";  // bilinear | nearest
        bool        integer = false;       // whole-number scaling only
        int         speed   = 100;         // emulation speed, percent
    };

    struct Audio {
        int         volume    = 100;   // percent, 0..200
        bool        muted     = false;
        std::string device;            // playback device name, "" = system default
        int         buffer_ms = 60;    // audio queued ahead of playback
    };

    // Game list presentation.
    struct Library {
        std::string view       = "grid";    // grid | list
        std::string sort_key   = "title";   // title | region | format | size | played | last
        bool        descending = false;
        int         cover_size = 220;       // grid cover edge in px
        bool        merge_discs = true;     // one entry per multi-disc game
        bool        auto_covers = true;     // fetch missing covers after each scan
        bool        watch_dirs  = true;     // rescan when the game folders change
        bool        box_covers  = false;    // grid covers drawn as 3D game cases
        std::vector<std::string> cover_urls;  // cover downloader templates
    };

    // Online play with friends (netplay) and Dreamcast Internet games.
    struct Network {
        bool        netplay_host  = true;   // last choice: host or join
        std::string netplay_peer;           // last address joined
        int         netplay_delay = 1;      // frames of input delay
        std::string dns;                    // Dreamcast DNS server, "" = engine's own
    };

    struct Window {
        int  width     = 1280;
        int  height    = 800;
        bool maximized = false;
    };

    Paths     paths;
    Graphics  graphics;
    Interface iface;
    Window    window;
    Behavior  behavior;
    Library   library;
    Network   network;
    Audio     audio;
    Display   display;
    Osd       osd;
    bool                   post_enabled = true;       // post-processing chain on/off
    bool                   post_native_input = false; // give shaders a 480-line image
    std::vector<PostStage> post_chain;                // stages, in order
    std::map<std::string, std::string> hotkeys;       // action id -> key name
    std::map<std::string, std::string> hotkey_pads;   // action id -> gamepad button name
    std::map<std::string, std::string> engine;        // engine option overrides
    std::map<std::string, std::string> bindings;      // "<port>.<input>" -> sources

    // XDG-aware paths
    static std::filesystem::path config_dir();
    static std::filesystem::path data_dir();
    static std::filesystem::path covers_dir();
    static std::filesystem::path cache_dir();      // can be deleted at any time
    static std::filesystem::path savestates_dir();
    static std::filesystem::path screenshots_dir();
    static std::filesystem::path gamesettings_dir();

    [[nodiscard]] static Config load();
    void save() const;
};

// Per-game overrides, layered over the global settings (DuckStation's
// per-game properties). Stored in <data>/gamesettings/<game id>.toml.
struct GameSettings {
    std::map<std::string, std::string> engine;   // engine option overrides

    [[nodiscard]] static GameSettings load(const std::string& game_id);
    void save(const std::string& game_id) const;
};

} // namespace ds::frontend
