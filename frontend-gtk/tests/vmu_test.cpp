// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "../src/vmu.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

using ds::frontend::VmuImage;

namespace {

// A DCI holding a save of `blocks` blocks with a recognisable payload.
std::vector<std::uint8_t> make_dci(const char* name, int blocks, std::uint8_t seed) {
    std::vector<std::uint8_t> dci(32, 0);
    dci[0] = 0x33;                                     // save data
    std::memset(dci.data() + 4, ' ', 12);
    std::memcpy(dci.data() + 4, name, std::strlen(name));
    dci[0x18] = static_cast<std::uint8_t>(blocks);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(blocks) * 512);
    for (std::size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<std::uint8_t>(seed + i * 7);
    std::memcpy(data.data(), "TEST SAVE       ", 16);  // short description
    for (std::size_t i = 0; i < data.size(); i += 4) {
        dci.push_back(data[i + 3]);
        dci.push_back(data[i + 2]);
        dci.push_back(data[i + 1]);
        dci.push_back(data[i]);
    }
    return dci;
}

} // namespace

TEST_CASE("a formatted VMU is valid and empty", "[vmu]") {
    const VmuImage v = VmuImage::formatted();
    REQUIRE(v.valid());
    REQUIRE(v.free_blocks() == 200);
    REQUIRE(v.files().empty());
}

TEST_CASE("files can be added, read back and removed", "[vmu]") {
    VmuImage v = VmuImage::formatted();
    std::string error;
    REQUIRE(v.import_dci(make_dci("SAVE_ONE.SYS", 4, 1), error));
    REQUIRE(v.import_dci(make_dci("SAVE_TWO.SYS", 9, 50), error));
    REQUIRE(v.free_blocks() == 200 - 13);

    const auto files = v.files();
    REQUIRE(files.size() == 2);
    REQUIRE(files[0].name == "SAVE_ONE.SYS");
    REQUIRE(files[0].blocks == 4);
    REQUIRE(files[0].first_block == 199);        // allocated from the top down
    REQUIRE(files[0].description == "TEST SAVE");
    REQUIRE(files[1].blocks == 9);

    const auto data = v.read(files[1]);
    REQUIRE(data.size() == 9 * 512);
    REQUIRE(data[600] == static_cast<std::uint8_t>(50 + 600 * 7));

    REQUIRE(v.remove(files[0]));
    REQUIRE(v.files().size() == 1);
    REQUIRE(v.free_blocks() == 200 - 9);
    // The survivor is untouched by the removal.
    REQUIRE(v.read(v.files()[0]) == data);
}

TEST_CASE("DCI export round-trips and copies between cards", "[vmu]") {
    VmuImage a = VmuImage::formatted();
    std::string error;
    const auto dci = make_dci("ROUNDTRIP.SV", 3, 9);
    REQUIRE(a.import_dci(dci, error));
    const auto out = a.export_dci(a.files()[0]);
    REQUIRE(out.size() == dci.size());
    // Everything but the directory's block pointer is preserved.
    REQUIRE(std::equal(out.begin() + 32, out.end(), dci.begin() + 32));

    VmuImage b = VmuImage::formatted();
    REQUIRE(b.copy_from(a, a.files()[0], error));
    REQUIRE(b.read(b.files()[0]) == a.read(a.files()[0]));
}

TEST_CASE("imports are refused when they cannot fit or would collide", "[vmu]") {
    VmuImage v = VmuImage::formatted();
    std::string error;
    REQUIRE(v.import_dci(make_dci("BIG.SAV", 150, 3), error));
    REQUIRE_FALSE(v.import_dci(make_dci("BIG.SAV", 2, 3), error));       // same name
    REQUIRE(error.find("already") != std::string::npos);
    REQUIRE_FALSE(v.import_dci(make_dci("TOO_BIG.SAV", 60, 3), error));  // 50 free
    REQUIRE(error.find("free") != std::string::npos);
    REQUIRE_FALSE(v.import_dci({1, 2, 3}, error));                        // garbage
    REQUIRE(v.files().size() == 1);
    REQUIRE(v.free_blocks() == 50);
}

TEST_CASE("an image survives a save and reload", "[vmu]") {
    VmuImage v = VmuImage::formatted();
    std::string error;
    REQUIRE(v.import_dci(make_dci("PERSIST.SAV", 5, 77), error));
    const auto file = std::filesystem::temp_directory_path() / "ds_vmu_test.bin";
    REQUIRE(v.save(file));
    const auto loaded = VmuImage::load(file);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->valid());
    REQUIRE(loaded->bytes() == v.bytes());
    std::filesystem::remove(file);
}

namespace {

// A VMI for `resource`.VMS naming `card_name` on the card.
std::vector<std::uint8_t> make_vmi(const char* resource, const char* card_name, bool game,
                                   bool protect, std::uint32_t size) {
    std::vector<std::uint8_t> vmi(108, 0);
    std::memcpy(vmi.data() + 0x04, "A test save", 11);
    vmi[0x44] = 2001 & 0xFF; vmi[0x45] = 2001 >> 8;
    vmi[0x46] = 3; vmi[0x47] = 14; vmi[0x48] = 21; vmi[0x49] = 7; vmi[0x4A] = 9;
    vmi[0x4B] = 3;                                    // Wednesday (0 = Sunday)
    vmi[0x4E] = 1;
    std::memcpy(vmi.data() + 0x50, resource, std::strlen(resource));
    std::memcpy(vmi.data() + 0x58, card_name, std::strlen(card_name));
    vmi[0x64] = static_cast<std::uint8_t>((game ? 2 : 0) | (protect ? 1 : 0));
    for (int i = 0; i < 4; ++i) vmi[0x68 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(size >> (8 * i));
    return vmi;
}

// A mini-game: code in block 0, the file header in block 1.
std::vector<std::uint8_t> make_game_vms(int blocks) {
    std::vector<std::uint8_t> vms(static_cast<std::size_t>(blocks) * 512, 0);
    vms[0] = 0x2A; vms[1] = 0x01; vms[2] = 0x02;    // not text
    std::memcpy(vms.data() + 0x200, "Tiny Game       ", 16);
    std::memcpy(vms.data() + 0x210, "A tiny VMU game                 ", 32);
    return vms;
}

} // namespace

TEST_CASE("VMS + VMI import and export round-trip", "[vmu]") {
    std::string error;
    std::vector<std::uint8_t> vms(3 * 512);
    for (std::size_t i = 0; i < vms.size(); ++i) vms[i] = static_cast<std::uint8_t>(i * 13);
    std::memcpy(vms.data(), "MY SAVE         ", 16);
    const auto vmi = make_vmi("MYSAVE", "MYGAME_SYS", false, true, 3 * 512);
    const auto in = VmuImage::parse_vms(vms, &vmi, "ignored", error);
    REQUIRE(in.has_value());
    REQUIRE(in->name() == "MYGAME_SYS");
    REQUIRE_FALSE(in->game());

    VmuImage v = VmuImage::formatted();
    REQUIRE(v.add(*in, false, error));
    const auto file = v.files().at(0);
    REQUIRE(file.copy_protected);
    REQUIRE(file.timestamp == "2001-03-14 21:07");
    REQUIRE(v.export_vms(file) == vms);

    const auto out = v.export_vmi(file, "MYSAVE");
    REQUIRE(VmuImage::vmi_resource({out.begin(), out.end()}) == "MYSAVE");
    // Checksum: the resource name ANDed with "SEGA".
    REQUIRE(out[0] == ('M' & 'S'));
    REQUIRE(out[3] == ('S' & 'A'));
    REQUIRE(out[0x64] == 1);                                   // data, protected
    REQUIRE(out[0x4B] == 3);                                   // weekday survives
    REQUIRE(std::equal(out.begin() + 0x44, out.begin() + 0x4B, vmi.begin() + 0x44));
    REQUIRE(std::memcmp(out.data() + 0x58, "MYGAME_SYS", 10) == 0);
}

TEST_CASE("a VMS without a VMI is typed from its header position", "[vmu]") {
    std::string error;
    const auto game = VmuImage::parse_vms(make_game_vms(4), nullptr, "tetris.vms", error);
    REQUIRE(game.has_value());
    REQUIRE(game->game());
    REQUIRE(game->name() == "TETRIS.VMS");

    std::vector<std::uint8_t> save(512, 0);
    std::memcpy(save.data(), "PLAIN SAVE      ", 16);
    const auto data = VmuImage::parse_vms(save, nullptr, "plain", error);
    REQUIRE(data.has_value());
    REQUIRE_FALSE(data->game());
}

TEST_CASE("replacing a save swaps it in, and keeps the card when it cannot", "[vmu]") {
    VmuImage v = VmuImage::formatted();
    std::string error;
    REQUIRE(v.import_dci(make_dci("FIGHTER.SYS", 10, 1), error));
    REQUIRE(v.import_dci(make_dci("FILLER.SYS", 150, 2), error));   // 40 free

    auto newer = VmuImage::parse_dci(make_dci("FIGHTER.SYS", 20, 9), error);
    REQUIRE(newer.has_value());
    REQUIRE_FALSE(v.add(*newer, false, error));                     // asks first
    REQUIRE(v.add(*newer, true, error));
    REQUIRE(v.find("FIGHTER.SYS")->blocks == 20);
    REQUIRE(v.read(*v.find("FIGHTER.SYS")) == newer->data);
    REQUIRE(v.free_blocks() == 30);

    const auto before = v.bytes();
    auto huge = VmuImage::parse_dci(make_dci("FIGHTER.SYS", 60, 4), error);
    REQUIRE_FALSE(v.add(*huge, true, error));                      // 50 would be free
    REQUIRE(v.bytes() == before);                                   // old save kept
}

TEST_CASE("one mini-game per card, placed from block 0", "[vmu]") {
    VmuImage v = VmuImage::formatted();
    std::string error;
    REQUIRE(v.import_dci(make_dci("SAVE.SYS", 5, 1), error));
    auto game = VmuImage::parse_vms(make_game_vms(6), nullptr, "game1", error);
    REQUIRE(v.add(*game, false, error));
    const auto g = v.find("GAME1");
    REQUIRE(g.has_value());
    REQUIRE(g->game);
    REQUIRE(g->first_block == 0);
    REQUIRE(g->header_block == 1);
    REQUIRE(g->description == "Tiny Game");
    auto other = VmuImage::parse_vms(make_game_vms(3), nullptr, "game2", error);
    REQUIRE_FALSE(v.add(*other, false, error));
    REQUIRE(error.find("mini-game") != std::string::npos);
}

TEST_CASE("a card saved as DCM reloads identically", "[vmu]") {
    VmuImage v = VmuImage::formatted();
    std::string error;
    REQUIRE(v.import_dci(make_dci("NEXUS.SAV", 7, 31), error));
    const auto file = std::filesystem::temp_directory_path() / "ds_vmu_test.dcm";
    REQUIRE(v.save(file));
    // On disk the bytes are reversed in fours: the root block signature is
    // still 0x55s, but a FAT entry's bytes trade places.
    const auto raw_dcm = VmuImage::load(std::filesystem::path(file).replace_extension(".dcm"));
    REQUIRE(raw_dcm.has_value());
    REQUIRE(raw_dcm->bytes() == v.bytes());
    std::ifstream in(file, std::ios::binary);
    std::vector<char> disk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(static_cast<std::uint8_t>(disk[254 * 512 + 3]) == v.bytes()[254 * 512 + 0]);
    std::filesystem::remove(file);
}
