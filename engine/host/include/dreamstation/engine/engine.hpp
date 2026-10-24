// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <functional>
#include <string>
#include <vector>

// Toolkit-independent host for the compiled-in Flycast engine fork
// (engine/flycast). Nothing here depends on GTK, Qt or SDL: the frontend
// supplies a GL context, a framebuffer, an audio sink and controller state.
//
// The engine is driven through its GUI-less shell entry points, which are
// process-global, so Engine is a singleton.
namespace ds::engine {

// Button bit positions (match the engine shell's joypad ids).
enum PadButton : unsigned {
    PAD_B = 0, PAD_Y, PAD_SELECT, PAD_START, PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
    PAD_A, PAD_X, PAD_L, PAD_R, PAD_L2, PAD_R2, PAD_L3, PAD_R3,
};

struct PadState {
    std::uint16_t buttons = 0;          // bitmask of (1 << PadButton)
    std::int16_t  lx = 0, ly = 0;       // -32768..32767
    std::int16_t  rx = 0, ry = 0;
    std::int16_t  l2 = 0, r2 = 0;       // analog triggers, 0..32767
};

enum class LogLevel { Debug = 0, Info, Warn, Error };

struct HostCallbacks {
    // GL framebuffer object the engine must render the final image into.
    std::function<std::uintptr_t()> current_framebuffer;
    // GL symbol lookup for the context that is current during run_frame().
    void* (*get_proc_address)(const char* sym) = nullptr;
    // Called once per run_frame(). `fresh` is false when the engine produced
    // no new image this call (the previous framebuffer content stays valid).
    std::function<void(unsigned width, unsigned height, bool fresh)> on_frame;
    // Interleaved stereo S16 at sample_rate(); `frames` = sample pairs.
    std::function<void(const std::int16_t* samples, std::size_t frames)> on_audio;
    // Called before the engine samples pads; refresh Engine::pad() here.
    std::function<void()> poll_input;
    std::function<void(LogLevel, const char* line)> on_log;
    // Vibration request for a port's rumble pack; strengths are 0..65535.
    std::function<void(unsigned port, std::uint16_t strong, std::uint16_t weak)> on_rumble;
    // Short user-facing notification (OSD/toast).
    std::function<void(const char* text)> on_message;
};

struct Paths {
    // Must contain dc/dc_boot.bin (+ dc/naomi.zip etc. for arcade boards).
    std::filesystem::path system_dir;
    // VMUs, NVRAM, per-game saves.
    std::filesystem::path save_dir;
};

struct OptionInfo {
    std::string key;                    // short key, as taken by set_option()
    std::string label;
    std::string info;                   // longer description, may be empty
    std::string category;               // engine category key, may be empty
    std::string category_label;
    std::string default_value;
    std::vector<std::string> values;
    std::vector<std::string> value_labels;   // same length as values
};

class Engine {
public:
    static Engine& instance();

    // Set an engine option before or after load. `key` is the short name
    // without the engine prefix, e.g. "internal_resolution" = "3840x2880".
    void set_option(const std::string& key, const std::string& value);
    const std::vector<OptionInfo>& options() const;

    bool init(const Paths& paths, HostCallbacks cb);
    // Empty path boots the console BIOS with no disc.
    bool load_game(const std::filesystem::path& game);
    // Call with the GL context current, after load_game() and whenever the
    // context is recreated.
    void context_reset();
    void context_destroy();
    // GL context must be current.
    void run_frame();
    void reset();
    void unload_game();
    void shutdown();

    bool game_loaded() const;
    bool shutdown_requested() const;

    double   refresh_rate() const;
    double   sample_rate() const;
    unsigned base_width() const;
    unsigned base_height() const;
    unsigned max_width() const;
    unsigned max_height() const;
    float    aspect_ratio() const;
    // True when the engine wants GL's bottom-left origin in the framebuffer.
    bool     bottom_left_origin() const;

    PadState& pad(unsigned port);
    // What is plugged into a controller port. Applies now if a game is
    // running and to every later game.
    enum class PortDevice { None, Controller, ArcadeStick, TwinStick, RacingWheel };
    void set_port_device(unsigned port, PortDevice device);
    void set_fast_forward(bool on);
    // For a running arcade game: what each pad button does in it, indexed by
    // PadButton ("Punch"; "Button 1" etc. for buttons it gives no name; ""
    // for buttons it does not use), and what its analog axes are (left X, Y,
    // right X, Y; "" for unused ones). nullopt for a Dreamcast game or none.
    struct ArcadeInputs {
        std::array<std::string, 16> buttons;
        std::array<std::string, 4>  axes;
    };
    std::optional<ArcadeInputs> arcade_inputs() const;

    bool save_state(const std::filesystem::path& file);
    bool load_state(const std::filesystem::path& file);
    // Disc drive. change_disc() opens the lid, swaps the image and closes it
    // again, as a player would; open_tray() leaves the drive empty.
    bool change_disc(const std::filesystem::path& image);
    bool open_tray();
    std::filesystem::path current_disc() const;

    // In-memory variants (undo buffer, rewind). Valid once a frame has run.
    bool serialize(std::vector<char>& out);
    bool unserialize(const std::vector<char>& in);

    // VMUs of the running game, by controller port (0..3) and slot (0, 1).
    // Call between frames.
    static constexpr int kVmuWidth = 48, kVmuHeight = 32;
    using VmuDots = std::array<std::uint8_t, kVmuWidth * kVmuHeight>;   // non-zero = lit
    // The VMU's screen as the game last drew it; false if it has drawn none.
    // `serial` changes with every new image.
    bool vmu_screen(unsigned port, unsigned slot, VmuDots& dots, std::uint64_t& serial) const;
    bool vmu_present(unsigned port, unsigned slot) const;
    // The card file that VMU reads and writes (valid while a game is loaded).
    std::filesystem::path vmu_path(unsigned port, unsigned slot) const;
    // Takes the VMU out (to play its mini-game) or plugs it back in; the
    // game notices within a few frames and, plugged back, reads the card
    // file again. Every VMU is plugged in again for the next game.
    void set_vmu_unplugged(unsigned port, unsigned slot, bool unplugged);

    // Online play (GGPO rollback netplay, two players). Set before
    // load_game(); nullopt plays offline. The host is player 1 and waits on
    // UDP port kNetplayPort; the other side joins its address. Both must load
    // the same game from the same files with the same saves and settings.
    static constexpr int kNetplayPort = 19713;
    struct Netplay {
        bool        host = true;
        std::string peer;          // when joining: "address" or "address:port"
        int         delay = 1;     // frames of input delay, 0..20
    };
    void set_netplay(const std::optional<Netplay>& netplay);
    enum class NetplayState { Off, Connecting, Running, Failed };
    // While Connecting, run_frame() runs nothing. On Failed, `message` says why.
    NetplayState netplay_state(std::string* message = nullptr) const;
    void cancel_netplay();
    // The DNS server Dreamcast games look their servers up with (empty =
    // the engine's own, which points games at community revival servers).
    void set_dns(const std::string& server);

private:
    Engine() = default;
};

// Full title of an arcade ROM set the engine knows ("mvsc2" -> "Marvel Vs.
// Capcom 2 ..."), or empty if the name is not a known set.
std::string arcade_title(const std::string& set_name);

// What the engine's table says about a known arcade ROM set.
struct ArcadeSet {
    struct Rom {
        std::string file_name;
        std::uint32_t length = 0;
        std::uint32_t crc = 0;
        bool optional = false;   // a default EEPROM the set can run without
    };
    std::string title;
    std::string board;    // "Naomi", "Naomi 2", "Atomiswave" or "System SP"
    std::string parent;   // set this one is a clone of, empty if none
    std::string gdrom;    // GD-ROM image the set boots from (<gdrom>.chd), if any
    std::vector<Rom> roms;
};
std::optional<ArcadeSet> arcade_set(const std::string& set_name);
// True if the file is that ROM, by size and checksum (reads the whole file).
bool arcade_rom_matches(const std::filesystem::path& file, const ArcadeSet::Rom& rom);

} // namespace ds::engine
