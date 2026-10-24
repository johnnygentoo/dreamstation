// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "../src/vmu_cpu.hpp"

#include <cmath>
#include <initializer_list>

using ds::frontend::VmuCpu;

namespace {

// A machine booted into `program` (assembled by hand, at address 0), with
// the first `game_blocks` blocks writable.
VmuCpu machine(std::initializer_list<std::uint8_t> program, int game_blocks = 1) {
    std::array<std::uint8_t, VmuCpu::kFlashSize> card{};
    std::size_t i = 0;
    for (auto b : program) card[i++] = b;
    VmuCpu cpu;
    cpu.load(card, game_blocks);
    cpu.boot(0);
    return cpu;
}

void steps(VmuCpu& cpu, int n) {
    for (int i = 0; i < n; ++i) cpu.step();
}

constexpr int ACC = 0x100, PSW = 0x101, B = 0x102, C = 0x103, SP = 0x106;

} // namespace

TEST_CASE("ADD sets carry, auxiliary carry and overflow", "[vmucpu]") {
    // mov #$7F,acc ; add #$01 ; add #$80
    auto cpu = machine({0x23, 0x00, 0x7F, 0x81, 0x01, 0x81, 0x80});
    steps(cpu, 2);
    REQUIRE(cpu.read(ACC) == 0x80);
    REQUIRE((cpu.read(PSW) & 0x80) == 0);       // no carry
    REQUIRE((cpu.read(PSW) & 0x40) != 0);       // carry out of bit 3
    REQUIRE((cpu.read(PSW) & 0x04) != 0);       // signed overflow
    REQUIRE((cpu.read(PSW) & 0x01) == 1);       // ACC has odd parity
    steps(cpu, 1);
    REQUIRE(cpu.read(ACC) == 0x00);
    REQUIRE((cpu.read(PSW) & 0x80) != 0);       // carry
}

TEST_CASE("SUBC borrows, and BE/BNE compare and set carry", "[vmucpu]") {
    // mov #$10,acc ; sub #$20 ; subc #$00 ; be #$EF,+2 ; nop ; nop ; bne #$EF,-2
    auto cpu = machine({0x23, 0x00, 0x10, 0xA1, 0x20, 0xB1, 0x00, 0x31, 0xEF, 0x02, 0x00, 0x00, 0x41, 0xEF, 0xFE});
    steps(cpu, 2);
    REQUIRE(cpu.read(ACC) == 0xF0);
    REQUIRE((cpu.read(PSW) & 0x80) != 0);       // borrow
    steps(cpu, 1);
    REQUIRE(cpu.read(ACC) == 0xEF);             // borrow taken off
    steps(cpu, 1);                              // be #$EF: taken, skips two NOPs
    REQUIRE(cpu.pc() == 12);
    REQUIRE((cpu.read(PSW) & 0x80) == 0);       // not less than
    steps(cpu, 1);                              // bne #$EF: equal, falls through
    REQUIRE(cpu.pc() == 15);
}

TEST_CASE("CALL / RET use the bank 0 stack, low byte first", "[vmucpu]") {
    // 0: call $010 ; 2: nop        $10: push acc ; pop b ; ret
    auto cpu = machine({0x08, 0x10, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                        0x61, 0x00, 0x71, 0x02, 0xA0});
    cpu.write(ACC, 0x5A);
    steps(cpu, 1);
    REQUIRE(cpu.pc() == 0x10);
    REQUIRE(cpu.read(SP) == 0x81);
    steps(cpu, 2);                              // push acc ; pop b
    REQUIRE(cpu.read(B) == 0x5A);
    steps(cpu, 1);                              // ret
    REQUIRE(cpu.pc() == 2);
    REQUIRE(cpu.read(SP) == 0x7F);
}

TEST_CASE("indirect addressing reaches RAM and SFRs through R0-R3", "[vmucpu]") {
    // mov #$40,@R0 ... R0 = cell 0 (bank 1, as booted) holds $30; @R2 uses cell 2 + $100
    auto cpu = machine({0x24, 0x40, 0x16, 0xC4});
    cpu.write(0x000, 0x30);
    cpu.write(0x002, 0x02);                     // @R2 -> $102 (B)
    steps(cpu, 1);                              // mov #$40,@R0
    REQUIRE(cpu.read(0x030) == 0x40);
    cpu.write(ACC, 0x77);
    steps(cpu, 1);                              // st @R2
    REQUIRE(cpu.read(B) == 0x77);
    cpu.write(ACC, 0x11);
    steps(cpu, 1);                              // xch @R0
    REQUIRE(cpu.read(ACC) == 0x40);
    REQUIRE(cpu.read(0x030) == 0x11);
}

TEST_CASE("LDC reads program memory at TRH:TRL + ACC", "[vmucpu]") {
    // mov #$10,trl ; mov #$00,trh ; mov #$02,acc ; ldc     data at $12
    auto cpu = machine({0x23, 0x04, 0x10, 0x23, 0x05, 0x00, 0x23, 0x00, 0x02, 0xC1,
                        0, 0, 0, 0, 0, 0, 0, 0, 0xAB});
    steps(cpu, 4);
    REQUIRE(cpu.read(ACC) == 0xAB);
}

TEST_CASE("MUL and DIV work on ACC:C and B", "[vmucpu]") {
    auto cpu = machine({0x30, 0x40});
    cpu.write(ACC, 0x12); cpu.write(C, 0x34); cpu.write(B, 0x10);
    steps(cpu, 1);                              // $1234 * $10 = $012340
    REQUIRE(cpu.read(B) == 0x01);
    REQUIRE(cpu.read(ACC) == 0x23);
    REQUIRE(cpu.read(C) == 0x40);
    REQUIRE((cpu.read(PSW) & 0x04) != 0);       // wider than 16 bits
    cpu.write(ACC, 0x12); cpu.write(C, 0x35); cpu.write(B, 0x10);
    steps(cpu, 1);                              // $1235 / $10 = $123 r 5
    REQUIRE(cpu.read(ACC) == 0x01);
    REQUIRE(cpu.read(C) == 0x23);
    REQUIRE(cpu.read(B) == 0x05);
}

TEST_CASE("DBNZ loops and bit branches test and clear bits", "[vmucpu]") {
    // mov #3,$20 ; loop: inc acc ; dbnz $20,loop ; bpc acc,1,+1 ; nop ; set1 b,7
    auto cpu = machine({0x22, 0x20, 0x03, 0x63, 0x00, 0x52, 0x20, 0xFB, 0x59, 0x00, 0x01, 0x00, 0xFF, 0x02});
    cpu.write(ACC, 0);
    steps(cpu, 1 + 3 * 2);
    REQUIRE(cpu.read(ACC) == 3);
    REQUIRE(cpu.pc() == 8);
    steps(cpu, 1);                              // bit 1 of 3 is set: cleared, branch
    REQUIRE(cpu.read(ACC) == 1);
    REQUIRE(cpu.pc() == 12);
    steps(cpu, 1);
    REQUIRE(cpu.read(B) == 0x80);
}

TEST_CASE("a firmware flash write lands in the game's own blocks only", "[vmucpu]") {
    // not1 ext,0 ; jmpf $100   (the game's own code would continue at $105)
    auto cpu = machine({0xB8, 0x0D, 0x21, 0x01, 0x00}, 2);
    cpu.write(0x07D, 0x00); cpu.write(0x07E, 0x02); cpu.write(0x07F, 0x00);   // address $200
    for (int i = 0; i < 128; ++i) cpu.write(0x080 + i, static_cast<std::uint8_t>(i));
    steps(cpu, 2);
    REQUIRE(cpu.pc() == 0x105);
    REQUIRE(cpu.read(ACC) == 0);
    REQUIRE(cpu.flash()[0x200 + 5] == 5);
    REQUIRE(cpu.flash_written());

    auto outside = machine({0xB8, 0x0D, 0x21, 0x01, 0x00}, 2);
    outside.write(0x07E, 0x04);                 // $400: past the 2-block game
    steps(outside, 2);
    REQUIRE(outside.read(ACC) == 0xFF);
    REQUIRE_FALSE(outside.flash_written());
}

TEST_CASE("leaving through the firmware's exit vector ends the game", "[vmucpu]") {
    auto cpu = machine({0xB8, 0x0D, 0x21, 0x01, 0xF0});
    cpu.run(0.01);
    REQUIRE(cpu.exited());
}

TEST_CASE("timer 1 drives the beeper at cycle rate / (256 - T1LR)", "[vmucpu]") {
    // 32768 / 6 = 5461 cycles a second; T1LR = $F0 gives a 16-count period
    // (341 Hz), T1LC = $F8 a half duty cycle.
    auto cpu = machine({0x01, 0xFE});           // br -2: spin
    cpu.write(0x11B, 0xF0);
    cpu.write(0x11A, 0xF8);
    cpu.write(0x118, 0x40);                     // T1LRUN
    cpu.set_audio_rate(48000);
    cpu.run(1.0);
    const auto samples = cpu.take_audio();
    REQUIRE(samples.size() > 47000);
    int crossings = 0, high = 0;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        crossings += (samples[i - 1] < 0) != (samples[i] < 0);
        high += samples[i] > 0;
    }
    const double hz = crossings / 2.0 / (samples.size() / 48000.0);
    REQUIRE(std::abs(hz - 5461.33 / 16) < 5);
    REQUIRE(std::abs(high / static_cast<double>(samples.size()) - 0.5) < 0.02);
}

TEST_CASE("buttons read active-low on P3 and raise the key interrupt flag", "[vmucpu]") {
    auto cpu = machine({0x00});
    REQUIRE(cpu.read(0x14C) == 0xFF);
    cpu.set_buttons(VmuCpu::A | VmuCpu::Up);
    REQUIRE(cpu.read(0x14C) == static_cast<std::uint8_t>(~0x11));
    REQUIRE((cpu.read(0x14E) & 0x02) != 0);
}
