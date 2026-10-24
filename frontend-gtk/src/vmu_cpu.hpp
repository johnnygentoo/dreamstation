// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ds::frontend {

// A Visual Memory Unit running a mini-game: the Sanyo LC86K87 CPU, its
// timers, the 48x32 LCD, the buttons and the beeper, with the card's 128 KB
// flash as program memory. Written from Marcus Comstedt's hardware notes
// (mc.pp.se/dc/vms).
//
// The VMU firmware is not needed. Games reach it only through a handful of
// entry points (flash read/write/verify, the clock tick, exit), and those
// are performed here directly, the way the firmware leaves the machine
// when it starts a game.
class VmuCpu {
public:
    static constexpr int kWidth = 48, kHeight = 32;
    static constexpr std::size_t kFlashSize = 128 * 1024;

    enum Button : unsigned {
        Up = 1u << 0, Down = 1u << 1, Left = 1u << 2, Right = 1u << 3,
        A = 1u << 4, B = 1u << 5, Mode = 1u << 6, Sleep = 1u << 7,
    };

    // `card`: the whole VMU image with the mini-game at block 0, occupying
    // `game_blocks` blocks (the only area a game may write to).
    void load(const std::array<std::uint8_t, kFlashSize>& card, int game_blocks);
    // Starts the game as the firmware would. `now` sets the VMU clock.
    void boot(std::int64_t unix_time);

    // Emulates `seconds` of real time.
    void run(double seconds);
    void set_buttons(unsigned pressed);   // Button bits

    bool exited() const { return m_exited; }
    bool display_on() const;
    // Pixel (x, y) of the dot matrix is lit.
    bool pixel(int x, int y) const;
    struct Icons { bool file, game, clock, flash; };
    Icons icons() const;

    const std::array<std::uint8_t, kFlashSize>& flash() const { return m_flash; }
    bool flash_written() const { return m_flash_written; }

    // Beeper output as signed 16-bit mono samples at `rate` Hz, produced
    // while running; take them with take_audio().
    void set_audio_rate(int rate) { m_audio_rate = rate; }
    std::vector<std::int16_t> take_audio();

    // Notes about things the emulation does not cover (unknown firmware
    // calls, undefined opcodes); each kind is reported once.
    std::function<void(const std::string&)> on_note;

    // For tests: direct access to the machine.
    std::uint8_t read(int address) const;           // 9-bit RAM/SFR space
    void         write(int address, std::uint8_t value);
    std::uint16_t pc() const { return m_pc; }
    void          set_pc(std::uint16_t pc) { m_pc = pc; }
    int           step();                           // one instruction; cycles used
    double        cycle_rate() const;               // CPU cycles per second now

private:
    // SFR addresses (offset from 0x100)
    enum : int {
        ACC = 0x00, PSW = 0x01, Breg = 0x02, Creg = 0x03, TRL = 0x04, TRH = 0x05, SP = 0x06,
        PCON = 0x07, IE = 0x08, IP = 0x09, EXT = 0x0D, OCR = 0x0E,
        T0CON = 0x10, T0PRR = 0x11, T0L = 0x12, T0LR = 0x13, T0H = 0x14, T0HR = 0x15,
        T1CNT = 0x18, T1LC = 0x1A, T1L = 0x1B, T1HC = 0x1C, T1H = 0x1D,
        MCR = 0x20, XBNK = 0x25, VCCR = 0x27,
        P1 = 0x44, P3 = 0x4C, P3INT = 0x4E, P7 = 0x5C, VSEL = 0x63, VRMAD1 = 0x64,
        VRMAD2 = 0x65, VTRBF = 0x66, BTCR = 0x7F,
    };

    std::uint8_t sfr(int index) const { return m_sfr[static_cast<std::size_t>(index)]; }
    std::uint8_t& sfr_ref(int index) { return m_sfr[static_cast<std::size_t>(index)]; }
    std::uint8_t rom(std::uint16_t address) const { return m_flash[address]; }
    std::uint8_t fetch() { return rom(m_pc++); }
    int indirect(int ri) const;
    void push(std::uint8_t v);
    std::uint8_t pop();
    void call(std::uint16_t target);
    void branch_if(bool cond, std::int8_t offset);
    void add(std::uint8_t v, bool carry);
    void sub(std::uint8_t v, bool carry);
    void compare_branch(bool equal_branch, std::uint8_t a, std::uint8_t b, std::int8_t offset);
    void set_flag(int bit, bool on);
    bool flag(int bit) const { return (sfr(PSW) >> bit) & 1; }

    void timers(int cycles);
    void tick_timer0();
    void interrupts();
    bool interrupt_pending() const;
    void firmware(std::uint16_t entry);
    void clock_tick();
    void note(const std::string& text);
    void audio(double seconds);

    std::array<std::uint8_t, kFlashSize> m_flash{};
    int  m_game_bytes = 0;
    bool m_flash_written = false;
    std::array<std::array<std::uint8_t, 256>, 2> m_ram{};
    std::array<std::uint8_t, 128> m_sfr{};
    std::array<std::array<std::uint8_t, 128>, 3> m_xram{};
    std::array<std::uint8_t, 512> m_work_ram{};
    std::uint16_t m_pc = 0;

    // Timer counters behind the SFR addresses that read as count and
    // write as reload.
    std::uint8_t m_t0_prescale = 0;
    std::uint8_t m_t1l = 0, m_t1h = 0, m_t1lr = 0, m_t1hr = 0;
    double       m_base_timer = 0;        // seconds towards the next half-second tick
    int          m_irq_depth = 0;         // interrupts being serviced
    unsigned     m_buttons = 0;
    bool         m_exited = false;

    int    m_audio_rate = 44100;
    double m_audio_phase = 0;             // position in the beeper's period, 0..1
    double m_audio_time = 0;              // seconds not yet turned into samples
    std::vector<std::int16_t> m_samples;

    std::vector<std::string> m_noted;
};

} // namespace ds::frontend
