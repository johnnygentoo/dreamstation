// SPDX-License-Identifier: GPL-3.0-or-later
#include "vmu_cpu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace ds::frontend {

namespace {

// PSW bits
constexpr int kCY = 7, kAC = 6, kOV = 2, kRAMBK0 = 1;

bool parity(std::uint8_t v) {
    v ^= static_cast<std::uint8_t>(v >> 4);
    v ^= static_cast<std::uint8_t>(v >> 2);
    v ^= static_cast<std::uint8_t>(v >> 1);
    return v & 1;
}

int days_in_month(int month, int year) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : kDays[std::clamp(month, 1, 12) - 1];
}

} // namespace

// ── Set-up ────────────────────────────────────────────────────────────────

void VmuCpu::load(const std::array<std::uint8_t, kFlashSize>& card, int game_blocks) {
    m_flash = card;
    m_game_bytes = std::clamp(game_blocks, 0, 200) * 512;
    m_flash_written = false;
}

void VmuCpu::boot(std::int64_t unix_time) {
    for (auto& bank : m_ram) bank.fill(0);
    m_sfr.fill(0);
    for (auto& bank : m_xram) bank.fill(0);
    m_work_ram.fill(0);
    m_t0_prescale = 0;
    m_t1l = m_t1h = m_t1lr = m_t1hr = 0;
    m_base_timer = 0;
    m_irq_depth = 0;
    m_exited = false;
    m_samples.clear();

    // The clock, kept by the firmware in RAM bank 0.
    const std::time_t t = static_cast<std::time_t>(unix_time);
    const std::tm tm = *std::localtime(&t);
    auto& sys = m_ram[0];
    const int year = tm.tm_year + 1900;
    sys[0x17] = static_cast<std::uint8_t>(year >> 8);
    sys[0x18] = static_cast<std::uint8_t>(year & 0xFF);
    sys[0x19] = static_cast<std::uint8_t>(tm.tm_mon + 1);
    sys[0x1A] = static_cast<std::uint8_t>(tm.tm_mday);
    sys[0x1B] = static_cast<std::uint8_t>(tm.tm_hour);
    sys[0x1C] = static_cast<std::uint8_t>(tm.tm_min);
    sys[0x1D] = static_cast<std::uint8_t>(tm.tm_sec);
    sys[0x1E] = 0;
    sys[0x1F] = days_in_month(2, year) == 29 ? 1 : 0;
    sys[0x31] = 0xFF;                                   // clock has been set
    sys[0x50] = static_cast<std::uint8_t>((year / 4) >> 8);
    sys[0x51] = static_cast<std::uint8_t>((year / 4) & 0xFF);
    sys[0x6D] = static_cast<std::uint8_t>(std::max(0, m_game_bytes / 512 - 1));

    // The state the firmware leaves when it jumps into a game: RAM bank 1
    // for the game, program memory from flash, LCD on, the half-second base
    // timer interrupt running, slow (32 kHz) clock.
    sfr_ref(PSW)  = 1u << kRAMBK0;
    sfr_ref(SP)   = 0x7F;
    sfr_ref(IE)   = 0x80;
    sfr_ref(EXT)  = 0x01;
    sfr_ref(OCR)  = 0xA3;
    sfr_ref(MCR)  = 0x09;
    sfr_ref(VCCR) = 0x80;
    sfr_ref(P3)   = 0xFF;
    sfr_ref(BTCR) = 0x41;
    m_pc = 0;
}

void VmuCpu::note(const std::string& text) {
    if (std::find(m_noted.begin(), m_noted.end(), text) != m_noted.end())
        return;
    m_noted.push_back(text);
    if (on_note)
        on_note(text);
}

double VmuCpu::cycle_rate() const {
    const std::uint8_t ocr = sfr(OCR);
    const double osc = (ocr & 0x20) ? 32768.0 : (ocr & 0x10) ? 6000000.0 : 600000.0;
    return osc / ((ocr & 0x80) ? 6.0 : 12.0);
}

// ── Memory ────────────────────────────────────────────────────────────────

std::uint8_t VmuCpu::read(int address) const {
    address &= 0x1FF;
    if (address < 0x100)
        return m_ram[(sfr(PSW) >> kRAMBK0) & 1][static_cast<std::size_t>(address)];
    if (address >= 0x180)
        return m_xram[std::min<std::size_t>(sfr(XBNK) & 3u, 2)][static_cast<std::size_t>(address - 0x180)];
    const int r = address - 0x100;
    switch (r) {
    case PSW:   return static_cast<std::uint8_t>((sfr(PSW) & 0xFE) | (parity(sfr(ACC)) ? 1 : 0));
    case T1L:   return m_t1l;
    case T1H:   return m_t1h;
    case P3:    return static_cast<std::uint8_t>(~m_buttons & 0xFF);
    case P7:    return 0x02;   // battery fine, not docked in a controller
    case VTRBF: {
        auto* self = const_cast<VmuCpu*>(this);
        const int at = ((sfr(VRMAD2) & 1) << 8) | sfr(VRMAD1);
        const std::uint8_t v = m_work_ram[static_cast<std::size_t>(at)];
        if (sfr(VSEL) & 0x10) {
            const int next = (at + 1) & 0x1FF;
            self->sfr_ref(VRMAD1) = static_cast<std::uint8_t>(next & 0xFF);
            self->sfr_ref(VRMAD2) = static_cast<std::uint8_t>(next >> 8);
        }
        return v;
    }
    default:    return sfr(r);
    }
}

void VmuCpu::write(int address, std::uint8_t value) {
    address &= 0x1FF;
    if (address < 0x100) {
        m_ram[(sfr(PSW) >> kRAMBK0) & 1][static_cast<std::size_t>(address)] = value;
        return;
    }
    if (address >= 0x180) {
        m_xram[std::min<std::size_t>(sfr(XBNK) & 3u, 2)][static_cast<std::size_t>(address - 0x180)] = value;
        return;
    }
    const int r = address - 0x100;
    switch (r) {
    case T1L: m_t1lr = value; return;          // writes go to the reload register
    case T1H: m_t1hr = value; return;
    case T1CNT:
        // A timer starting loads its reload value.
        if ((value & 0x40) && !(sfr(T1CNT) & 0x40)) m_t1l = m_t1lr;
        if ((value & 0x80) && !(sfr(T1CNT) & 0x80)) m_t1h = m_t1hr;
        break;
    case VTRBF: {
        const int at = ((sfr(VRMAD2) & 1) << 8) | sfr(VRMAD1);
        m_work_ram[static_cast<std::size_t>(at)] = value;
        if (sfr(VSEL) & 0x10) {
            const int next = (at + 1) & 0x1FF;
            sfr_ref(VRMAD1) = static_cast<std::uint8_t>(next & 0xFF);
            sfr_ref(VRMAD2) = static_cast<std::uint8_t>(next >> 8);
        }
        return;
    }
    default:
        break;
    }
    sfr_ref(r) = value;
}

int VmuCpu::indirect(int ri) const {
    const int cell = ((sfr(PSW) >> 3) & 3) * 4 + ri;
    return read(cell) | ((ri & 2) ? 0x100 : 0);
}

void VmuCpu::push(std::uint8_t v) {
    sfr_ref(SP) = static_cast<std::uint8_t>(sfr(SP) + 1);
    m_ram[0][sfr(SP)] = v;                    // the stack is always in bank 0
}

std::uint8_t VmuCpu::pop() {
    const std::uint8_t v = m_ram[0][sfr(SP)];
    sfr_ref(SP) = static_cast<std::uint8_t>(sfr(SP) - 1);
    return v;
}

void VmuCpu::call(std::uint16_t target) {
    push(static_cast<std::uint8_t>(m_pc & 0xFF));
    push(static_cast<std::uint8_t>(m_pc >> 8));
    m_pc = target;
}

void VmuCpu::branch_if(bool cond, std::int8_t offset) {
    if (cond)
        m_pc = static_cast<std::uint16_t>(m_pc + offset);
}

void VmuCpu::set_flag(int bit, bool on) {
    std::uint8_t& psw = sfr_ref(PSW);
    psw = static_cast<std::uint8_t>(on ? (psw | (1u << bit)) : (psw & ~(1u << bit)));
}

void VmuCpu::add(std::uint8_t v, bool carry) {
    const int a = sfr(ACC), c = carry && flag(kCY) ? 1 : 0;
    const int r = a + v + c;
    set_flag(kCY, r > 0xFF);
    set_flag(kAC, (a & 0xF) + (v & 0xF) + c > 0xF);
    set_flag(kOV, ((a ^ r) & (v ^ r) & 0x80) != 0);
    sfr_ref(ACC) = static_cast<std::uint8_t>(r);
}

void VmuCpu::sub(std::uint8_t v, bool carry) {
    const int a = sfr(ACC), c = carry && flag(kCY) ? 1 : 0;
    const int r = a - v - c;
    set_flag(kCY, r < 0);
    set_flag(kAC, (a & 0xF) - (v & 0xF) - c < 0);
    set_flag(kOV, ((a ^ v) & (a ^ r) & 0x80) != 0);
    sfr_ref(ACC) = static_cast<std::uint8_t>(r);
}

void VmuCpu::compare_branch(bool equal_branch, std::uint8_t a, std::uint8_t b, std::int8_t offset) {
    set_flag(kCY, a < b);
    branch_if((a == b) == equal_branch, offset);
}

// ── Instructions ──────────────────────────────────────────────────────────

int VmuCpu::step() {
    const std::uint8_t op = fetch();
    const int hi = op >> 4, lo = op & 0x0F;
    const auto r8 = [this] { return static_cast<std::int8_t>(fetch()); };

    if (lo >= 8) {
        // Bit operations on a direct address, or 12-bit CALL / JMP.
        if (hi <= 3) {
            const std::uint16_t a12 = static_cast<std::uint16_t>(((op & 0x10) << 7) | ((op & 7) << 8) | fetch());
            const std::uint16_t target = static_cast<std::uint16_t>((m_pc & 0xF000) | a12);
            if (hi <= 1) call(target); else m_pc = target;
            return 2;
        }
        const int d9 = ((op & 0x10) << 4) | fetch();
        const std::uint8_t mask = static_cast<std::uint8_t>(1u << (op & 7));
        const std::uint8_t v = read(d9);
        switch (hi) {
        case 0x4: case 0x5: {                               // BPC
            const std::int8_t off = r8();
            if (v & mask) { write(d9, static_cast<std::uint8_t>(v & ~mask)); branch_if(true, off); }
            return 2;
        }
        case 0x6: case 0x7: branch_if(v & mask, r8()); return 2;          // BP
        case 0x8: case 0x9: branch_if(!(v & mask), r8()); return 2;       // BN
        case 0xA: case 0xB: write(d9, static_cast<std::uint8_t>(v ^ mask)); return 1;   // NOT1
        case 0xC: case 0xD: write(d9, static_cast<std::uint8_t>(v & ~mask)); return 1;  // CLR1
        default:            write(d9, static_cast<std::uint8_t>(v | mask)); return 1;   // SET1
        }
    }

    if (lo >= 2) {
        // An operand in memory: direct (lo 2-3) or indirect (lo 4-7).
        int address;
        if (lo >= 4) address = indirect(op & 3);
        else         address = ((op & 1) << 8) | fetch();
        const bool direct = lo < 4;
        switch (hi) {
        case 0x0: sfr_ref(ACC) = read(address); return 1;                       // LD
        case 0x1: write(address, sfr(ACC)); return 1;                           // ST
        case 0x2: write(address, fetch()); return direct ? 2 : 1;               // MOV #i8
        case 0x3: case 0x4: {                                                   // BE / BNE
            if (direct) {
                const std::uint8_t v = read(address);
                compare_branch(hi == 3, sfr(ACC), v, r8());
            } else {
                const std::uint8_t imm = fetch();
                compare_branch(hi == 3, read(address), imm, r8());
            }
            return 2;
        }
        case 0x5: {                                                              // DBNZ
            const std::uint8_t v = static_cast<std::uint8_t>(read(address) - 1);
            write(address, v);
            branch_if(v != 0, r8());
            return 2;
        }
        case 0x6: write(address, static_cast<std::uint8_t>(read(address) + 1)); return 1;   // INC
        case 0x7: write(address, static_cast<std::uint8_t>(read(address) - 1)); return 1;   // DEC
        case 0x8: add(read(address), false); return 1;
        case 0x9: add(read(address), true); return 1;
        case 0xA: sub(read(address), false); return 1;
        case 0xB: sub(read(address), true); return 1;
        case 0xC: {                                                              // XCH
            const std::uint8_t v = read(address);
            write(address, sfr(ACC));
            sfr_ref(ACC) = v;
            return 1;
        }
        case 0xD: sfr_ref(ACC) = static_cast<std::uint8_t>(sfr(ACC) | read(address)); return 1;
        case 0xE: sfr_ref(ACC) = static_cast<std::uint8_t>(sfr(ACC) & read(address)); return 1;
        default:  sfr_ref(ACC) = static_cast<std::uint8_t>(sfr(ACC) ^ read(address)); return 1;
        }
    }

    if (hi == 6 || hi == 7) {
        // PUSH / POP d9 (the low bit of the opcode is d8).
        const int d9 = ((op & 1) << 8) | fetch();
        if (hi == 6) push(read(d9)); else write(d9, pop());
        return 2;
    }

    if (lo == 1) {
        switch (hi) {
        case 0x0: branch_if(true, r8()); return 2;                              // BR
        case 0x1: {                                                              // BRF
            const std::uint8_t l = fetch(), h = fetch();
            m_pc = static_cast<std::uint16_t>(m_pc - 1 + ((h << 8) | l));
            return 4;
        }
        case 0x2: {                                                              // JMPF
            const std::uint8_t h = fetch(), l = fetch();
            m_pc = static_cast<std::uint16_t>((h << 8) | l);
            // NOT1 EXT,0 followed by JMPF is how a game calls the firmware:
            // the switch to the firmware's memory takes effect at the jump.
            if (!(sfr(EXT) & 1))
                firmware(m_pc);
            return 2;
        }
        case 0x3: case 0x4: {                                                    // BE / BNE #i8
            const std::uint8_t imm = fetch();
            compare_branch(hi == 3, sfr(ACC), imm, r8());
            return 2;
        }
        case 0x8: add(fetch(), false); return 1;
        case 0x9: add(fetch(), true); return 1;
        case 0xA: sub(fetch(), false); return 1;
        case 0xB: sub(fetch(), true); return 1;
        case 0xC: {                                                              // LDC
            const std::uint16_t at = static_cast<std::uint16_t>(((sfr(TRH) << 8) | sfr(TRL)) + sfr(ACC));
            sfr_ref(ACC) = rom(at);
            return 2;
        }
        case 0xD: sfr_ref(ACC) = static_cast<std::uint8_t>(sfr(ACC) | fetch()); return 1;
        case 0xE: sfr_ref(ACC) = static_cast<std::uint8_t>(sfr(ACC) & fetch()); return 1;
        case 0xF: sfr_ref(ACC) = static_cast<std::uint8_t>(sfr(ACC) ^ fetch()); return 1;
        default: break;
        }
    } else {   // lo == 0
        switch (hi) {
        case 0x0: return 1;                                                      // NOP
        case 0x1: {                                                              // CALLR
            const std::uint8_t l = fetch(), h = fetch();
            call(static_cast<std::uint16_t>(m_pc - 1 + ((h << 8) | l)));
            return 4;
        }
        case 0x2: {                                                              // CALLF
            const std::uint8_t h = fetch(), l = fetch();
            call(static_cast<std::uint16_t>((h << 8) | l));
            return 2;
        }
        case 0x3: {                                                              // MUL
            const std::uint32_t v = static_cast<std::uint32_t>((sfr(ACC) << 8) | sfr(Creg)) * sfr(Breg);
            sfr_ref(Breg) = static_cast<std::uint8_t>(v >> 16);
            sfr_ref(ACC)  = static_cast<std::uint8_t>(v >> 8);
            sfr_ref(Creg) = static_cast<std::uint8_t>(v);
            set_flag(kCY, false);
            set_flag(kOV, v > 0xFFFF);
            return 7;
        }
        case 0x4: {                                                              // DIV
            const std::uint16_t v = static_cast<std::uint16_t>((sfr(ACC) << 8) | sfr(Creg));
            set_flag(kCY, false);
            if (sfr(Breg) == 0) {
                set_flag(kOV, true);
                return 7;
            }
            const std::uint16_t q = static_cast<std::uint16_t>(v / sfr(Breg));
            sfr_ref(Breg) = static_cast<std::uint8_t>(v % sfr(Breg));
            sfr_ref(ACC)  = static_cast<std::uint8_t>(q >> 8);
            sfr_ref(Creg) = static_cast<std::uint8_t>(q);
            set_flag(kOV, false);
            return 7;
        }
        case 0x8: branch_if(sfr(ACC) == 0, r8()); return 2;                    // BZ
        case 0x9: branch_if(sfr(ACC) != 0, r8()); return 2;                    // BNZ
        case 0xA: case 0xB: {                                                    // RET / RETI
            const std::uint8_t h = pop(), l = pop();
            m_pc = static_cast<std::uint16_t>((h << 8) | l);
            if (hi == 0xB && m_irq_depth > 0)
                --m_irq_depth;
            return 2;
        }
        case 0xC: {                                                              // ROR
            const std::uint8_t a = sfr(ACC);
            sfr_ref(ACC) = static_cast<std::uint8_t>((a >> 1) | (a << 7));
            return 1;
        }
        case 0xD: {                                                              // RORC
            const std::uint8_t a = sfr(ACC);
            sfr_ref(ACC) = static_cast<std::uint8_t>((a >> 1) | (flag(kCY) ? 0x80 : 0));
            set_flag(kCY, a & 1);
            return 1;
        }
        case 0xE: {                                                              // ROL
            const std::uint8_t a = sfr(ACC);
            sfr_ref(ACC) = static_cast<std::uint8_t>((a << 1) | (a >> 7));
            return 1;
        }
        case 0xF: {                                                              // ROLC
            const std::uint8_t a = sfr(ACC);
            sfr_ref(ACC) = static_cast<std::uint8_t>((a << 1) | (flag(kCY) ? 1 : 0));
            set_flag(kCY, a & 0x80);
            return 1;
        }
        default: break;
        }
    }
    char text[64];
    std::snprintf(text, sizeof(text), "undefined opcode %02X at %04X (treated as NOP)", op,
                  static_cast<unsigned>(m_pc - 1));
    note(text);
    return 1;
}

// ── Firmware ──────────────────────────────────────────────────────────────

void VmuCpu::firmware(std::uint16_t entry) {
    auto& io = m_ram[1];      // the calls' variables live in RAM bank 1
    const std::uint32_t address = ((static_cast<std::uint32_t>(io[0x7D]) << 16) |
                                   (static_cast<std::uint32_t>(io[0x7E]) << 8) | io[0x7F]) &
                                  (kFlashSize - 1) & ~0x7Fu;
    std::uint16_t back = 0;
    switch (entry) {
    case 0x100: case 0x108:                                   // write 128 bytes
        if (address + 128 <= static_cast<std::uint32_t>(m_game_bytes)) {
            std::copy_n(io.begin() + 0x80, 128, m_flash.begin() + address);
            m_flash_written = true;
            sfr_ref(ACC) = 0;
        } else {
            sfr_ref(ACC) = 0xFF;   // only the mini-game's own file may be written
        }
        back = static_cast<std::uint16_t>(entry + 5);
        break;
    case 0x110:                                               // verify 128 bytes
        sfr_ref(ACC) = std::equal(io.begin() + 0x80, io.end(), m_flash.begin() + address) ? 0 : 1;
        back = 0x115;
        break;
    case 0x120:                                               // read 128 bytes
        std::copy_n(m_flash.begin() + address, 128, io.begin() + 0x80);
        back = 0x125;
        break;
    case 0x130:                                               // clock housekeeping
        clock_tick();
        back = 0x139;
        break;
    case 0x1F0:                                               // leave the game
        m_exited = true;
        return;
    default: {
        char text[64];
        std::snprintf(text, sizeof(text), "firmware call %04X is not emulated; the game was stopped",
                      static_cast<unsigned>(entry));
        note(text);
        m_exited = true;
        return;
    }
    }
    sfr_ref(EXT) = static_cast<std::uint8_t>(sfr(EXT) | 1);   // back to the game in flash
    m_pc = back;
}

void VmuCpu::clock_tick() {
    // Called on each half-second base timer interrupt: advance the clock.
    sfr_ref(BTCR) = static_cast<std::uint8_t>(sfr(BTCR) & ~0x02);
    auto& sys = m_ram[0];
    sys[0x1E] ^= 1;
    if (sys[0x1E] != 0)
        return;
    if (++sys[0x1D] < 60) return;
    sys[0x1D] = 0;
    if (++sys[0x1C] < 60) return;
    sys[0x1C] = 0;
    if (++sys[0x1B] < 24) return;
    sys[0x1B] = 0;
    int year = (sys[0x17] << 8) | sys[0x18];
    if (++sys[0x1A] <= days_in_month(sys[0x19], year)) return;
    sys[0x1A] = 1;
    if (++sys[0x19] <= 12) return;
    sys[0x19] = 1;
    ++year;
    sys[0x17] = static_cast<std::uint8_t>(year >> 8);
    sys[0x18] = static_cast<std::uint8_t>(year & 0xFF);
    sys[0x1F] = days_in_month(2, year) == 29 ? 1 : 0;
    sys[0x50] = static_cast<std::uint8_t>((year / 4) >> 8);
    sys[0x51] = static_cast<std::uint8_t>((year / 4) & 0xFF);
}

// ── Timers and interrupts ─────────────────────────────────────────────────

void VmuCpu::tick_timer0() {
    std::uint8_t& con = sfr_ref(T0CON);
    if (con & 0x20) {                         // 16-bit mode
        if (!(con & 0x40)) return;
        if (++sfr_ref(T0L) != 0) return;
        sfr_ref(T0L) = sfr(T0LR);
        if (++sfr_ref(T0H) != 0) return;
        sfr_ref(T0H) = sfr(T0HR);
        con |= 0x08;
        return;
    }
    if ((con & 0x40) && ++sfr_ref(T0L) == 0) {
        sfr_ref(T0L) = sfr(T0LR);
        con |= 0x02;
    }
    if ((con & 0x80) && ++sfr_ref(T0H) == 0) {
        sfr_ref(T0H) = sfr(T0HR);
        con |= 0x08;
    }
}

void VmuCpu::timers(int cycles) {
    std::uint8_t& t1 = sfr_ref(T1CNT);
    for (int i = 0; i < cycles; ++i) {
        if (sfr(T0CON) & 0xC0) {
            if (++m_t0_prescale == 0) {
                m_t0_prescale = sfr(T0PRR);
                tick_timer0();
            }
        }
        if (t1 & 0x40) {
            if (++m_t1l == 0) {
                m_t1l = m_t1lr;
                t1 |= 0x02;
                if ((t1 & 0x20) && (t1 & 0x80) && ++m_t1h == 0) {
                    m_t1h = m_t1hr;
                    t1 |= 0x08;
                }
            }
        }
        if ((t1 & 0x80) && !(t1 & 0x20) && ++m_t1h == 0) {
            m_t1h = m_t1hr;
            t1 |= 0x08;
        }
    }
}

bool VmuCpu::interrupt_pending() const {
    const std::uint8_t t0 = sfr(T0CON), t1 = sfr(T1CNT), bt = sfr(BTCR);
    return ((t0 & 0x03) == 0x03) || ((t0 & 0x0C) == 0x0C) || ((bt & 0x03) == 0x03) ||
           ((t1 & 0x03) == 0x03) || ((t1 & 0x0C) == 0x0C) || ((sfr(P3INT) & 0x03) == 0x03);
}

void VmuCpu::interrupts() {
    if (!interrupt_pending())
        return;
    sfr_ref(PCON) = static_cast<std::uint8_t>(sfr(PCON) & ~0x01);   // a request ends HALT
    if (!(sfr(IE) & 0x80) || m_irq_depth > 0)
        return;
    const std::uint8_t t0 = sfr(T0CON), t1 = sfr(T1CNT), bt = sfr(BTCR);
    std::uint16_t vector = 0;
    if ((t0 & 0x03) == 0x03)                                  vector = 0x13;
    else if ((bt & 0x03) == 0x03)                             vector = 0x1B;
    else if ((t0 & 0x0C) == 0x0C)                             vector = 0x23;
    else if ((t1 & 0x03) == 0x03 || (t1 & 0x0C) == 0x0C)      vector = 0x2B;
    else                                                      vector = 0x4B;
    ++m_irq_depth;
    call(vector);
}

void VmuCpu::set_buttons(unsigned pressed) {
    const unsigned newly = pressed & ~m_buttons;
    m_buttons = pressed & 0xFF;
    if (newly) {
        sfr_ref(P3INT) = static_cast<std::uint8_t>(sfr(P3INT) | 0x02);
        if (sfr(P3INT) & 0x04)                                // keys may end HOLD
            sfr_ref(PCON) = static_cast<std::uint8_t>(sfr(PCON) & ~0x02);
    }
}

// ── Running ───────────────────────────────────────────────────────────────

void VmuCpu::audio(double seconds) {
    if (m_audio_rate <= 0)
        return;
    const double step = 1.0 / m_audio_rate;
    m_audio_time += seconds;
    // The beeper follows timer 1 low: high while the count is at or above
    // T1LC, over a period of (256 - T1LR) counts.
    const std::uint8_t t1 = sfr(T1CNT);
    const int period = 256 - m_t1lr;
    const double duty = period > 0 ? (256.0 - std::max<int>(sfr(T1LC), m_t1lr)) / period : 0.0;
    const bool sounding = (t1 & 0x40) && period > 1 && duty > 0.0 && duty < 1.0;
    const double freq = sounding ? cycle_rate() / period : 0.0;
    while (m_audio_time >= step) {
        m_audio_time -= step;
        std::int16_t s = 0;
        if (sounding && freq < m_audio_rate / 2.0) {
            m_audio_phase += freq * step;
            m_audio_phase -= std::floor(m_audio_phase);
            s = m_audio_phase < duty ? 5000 : -5000;
        }
        if (m_samples.size() < static_cast<std::size_t>(m_audio_rate))
            m_samples.push_back(s);
    }
}

std::vector<std::int16_t> VmuCpu::take_audio() {
    std::vector<std::int16_t> out;
    out.swap(m_samples);
    return out;
}

void VmuCpu::run(double seconds) {
    double t = 0;
    while (t < seconds && !m_exited) {
        if (sfr(PCON) & 0x02) {          // HOLD: everything stops until a key
            audio(seconds - t);
            return;
        }
        interrupts();
        const int cycles = (sfr(PCON) & 0x01) ? 1 : step();   // HALT: only timers run
        const double dt = cycles / cycle_rate();
        timers(cycles);
        // The base timer runs from the 32 kHz crystal: a tick every half second.
        if (sfr(BTCR) & 0x40) {
            m_base_timer += dt;
            while (m_base_timer >= 0.5) {
                m_base_timer -= 0.5;
                sfr_ref(BTCR) = static_cast<std::uint8_t>(sfr(BTCR) | 0x02);
            }
        }
        audio(dt);
        t += dt;
    }
}

// ── Display ───────────────────────────────────────────────────────────────

bool VmuCpu::display_on() const { return (sfr(VCCR) & 0x80) != 0; }

bool VmuCpu::pixel(int x, int y) const {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight)
        return false;
    // Each bank holds 16 lines; a pair of lines takes 12 bytes, then 4 are skipped.
    const int row = y % 16;
    const int at = (row / 2) * 0x10 + (row % 2) * 6 + x / 8;
    return (m_xram[static_cast<std::size_t>(y / 16)][static_cast<std::size_t>(at)] >> (7 - x % 8)) & 1;
}

VmuCpu::Icons VmuCpu::icons() const {
    const auto& b = m_xram[2];
    return {(b[0x01] & 0x40) != 0, (b[0x02] & 0x10) != 0, (b[0x03] & 0x04) != 0, (b[0x04] & 0x01) != 0};
}

} // namespace ds::frontend
