// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gtkmm.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "../vmu.hpp"
#include "../vmu_cpu.hpp"

struct SDL_AudioStream;

namespace ds::frontend {

// A Visual Memory Unit drawn like the real one: the LCD with its four status
// icons and the buttons, which can be clicked, pressed on the keyboard or on
// a gamepad. It either mirrors the screen a running Dreamcast game draws on
// the VMU, or plays a mini-game (Chao Adventure, homebrew, ...) itself.
//
// A mini-game comes from a card image (the game at block 0) or from a .vms
// or .dci file. Whatever it writes to its flash is saved back to that source
// when it exits or stop() is called, as the VMU would keep it.
class VmuUnit : public Gtk::Box {
public:
    // `volume` 0..200 percent.
    explicit VmuUnit(int volume);
    ~VmuUnit() override;

    // Starts the mini-game in `source`; false with `error` set if there is
    // none to run.
    bool play(const std::filesystem::path& source, std::string& error);
    // Saves the mini-game's progress and goes back to showing `screen`.
    void stop();
    bool playing() const { return m_playing; }
    const std::filesystem::path& source() const { return m_path; }
    // The mini-game's name as its file describes it.
    std::string game_title() const;

    // What the LCD shows when no mini-game runs: a game's VMU screen
    // (48x32 dots, non-zero = lit), or nothing (null).
    void show_screen(const std::uint8_t* dots);

    // Keys go to the unit from its window: true if the key is a VMU button.
    bool key(guint keyval, bool pressed);
    // For scripted test runs: hold `buttons` (VmuCpu::Button bits).
    void test_hold(unsigned buttons) { m_test_buttons = buttons; }

    void set_status(const std::string& text) { m_status.set_text(text); }

    // Emitted when the mini-game ends by itself (its exit, e.g. MODE).
    sigc::signal<void()> signal_ended;

private:
    enum class Source { Card, Vms, Dci };

    bool load(const std::filesystem::path& source, std::string& error);
    void start();
    bool on_tick(const Glib::RefPtr<Gdk::FrameClock>& clock);
    void draw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height);
    unsigned pad_buttons() const;
    void persist();
    void open_audio();
    void close_audio();

    std::filesystem::path m_path;
    Source         m_source = Source::Card;
    VmuImage       m_card;
    VmuImage::File m_game;
    std::array<std::uint8_t, 32> m_dci_entry{};
    std::size_t    m_vms_size = 0;
    std::vector<std::uint8_t> m_saved;      // game bytes as last saved
    VmuCpu         m_cpu;
    bool           m_playing = false;
    bool           m_ended = false;

    bool m_has_screen = false;              // mirroring a game's screen
    std::array<std::uint8_t, VmuCpu::kWidth * VmuCpu::kHeight> m_screen{};

    Gtk::DrawingArea m_area;
    Gtk::Label       m_status;
    guint            m_tick_id = 0;          // frame callback while playing

    std::int64_t m_last_frame = 0;
    unsigned     m_key_buttons = 0, m_mouse_buttons = 0, m_test_buttons = 0;
    std::map<unsigned, Gdk::Rectangle> m_button_rects;   // where each button is drawn

    SDL_AudioStream* m_audio = nullptr;
    float            m_gain = 1.f;
};

} // namespace ds::frontend
