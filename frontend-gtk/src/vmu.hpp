// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ds::frontend {

// A Dreamcast Visual Memory Unit image: 256 blocks of 512 bytes (128 KB).
// Block 255 is the root block, 254 the FAT, 253 downward the directory;
// blocks 0..199 hold the user's files, save data allocated from the top
// down and a mini-game from block 0 up.
//
// This class reads and edits the image as a file system: list, extract,
// add, replace and delete files, in the exchange formats people trade saves
// in. It knows nothing about a running emulator, so an image must not be
// edited while a game has it plugged in.
class VmuImage {
public:
    static constexpr std::size_t kBlockSize  = 512;
    static constexpr std::size_t kBlocks     = 256;
    static constexpr std::size_t kImageSize  = kBlockSize * kBlocks;
    static constexpr int         kUserBlocks = 200;
    static constexpr std::size_t kVmiSize    = 108;

    // How a whole card is stored on disk.
    enum class CardFormat {
        Raw,    // a plain dump (.bin, .vmu): what the engine uses
        Dcm,    // Nexus memory card dump: bytes reversed in groups of four
    };

    struct File {
        int         dir_index = 0;       // slot in the directory
        bool        game = false;        // a VMU mini-game rather than save data
        bool        copy_protected = false;
        std::string name;                // 12-character file name on the card
        std::string description;         // short description (VMU file menu)
        std::string long_description;    // description shown by the Dreamcast
        std::string app_id;              // application that created the file
        std::string timestamp;           // "2001-03-14 21:07"
        int         first_block = 0;
        int         blocks = 0;
        int         header_block = 0;    // offset of the header within the file
        // Icon: 32x32 RGBA frames (one, or up to three when animated) and
        // the delay between frames in 1/30 s units. Empty if the file has none.
        std::vector<std::vector<std::uint8_t>> icons;
        int         icon_speed = 0;
        // Optional 72x56 RGBA picture shown by the Dreamcast file manager.
        std::vector<std::uint8_t> eyecatch;
        static constexpr int kEyecatchWidth = 72, kEyecatchHeight = 56;

        // First icon frame, for code that shows a still icon.
        const std::vector<std::uint8_t>& icon() const;
    };

    // A file outside any card, as read from an exchange format: its
    // directory entry (first block ignored) and its blocks.
    struct Incoming {
        std::array<std::uint8_t, 32> entry{};
        std::vector<std::uint8_t>    data;
        std::string name() const;
        bool        game() const { return entry[0] == 0xCC; }
        int         blocks() const { return static_cast<int>((data.size() + kBlockSize - 1) / kBlockSize); }
    };

    // A freshly formatted, empty card.
    static VmuImage formatted();
    // Reads a raw or DCM card image (DCM chosen by the .dcm extension).
    static std::optional<VmuImage> load(const std::filesystem::path& file);
    static VmuImage from_bytes(const std::array<std::uint8_t, kImageSize>& bytes);
    bool save(const std::filesystem::path& file) const;   // format by extension
    bool save(const std::filesystem::path& file, CardFormat format) const;
    static CardFormat format_for(const std::filesystem::path& file);

    bool valid() const;                  // has the root-block signature
    std::vector<File> files() const;
    std::optional<File> find(const std::string& name) const;
    int free_blocks() const;
    // The largest mini-game that fits: free blocks in one run from block 0.
    int free_game_blocks() const;

    // Raw contents of a file (blocks * 512 bytes).
    std::vector<std::uint8_t> read(const File& file) const;
    bool remove(const File& file);
    bool set_copy_protected(const File& file, bool on);

    // Adds a file. When a file of that name exists already it is replaced
    // if `replace` is set, otherwise the call fails. Nothing changes on
    // failure; `error` says why.
    bool add(const Incoming& file, bool replace, std::string& error);
    Incoming take(const File& file) const;

    // DCI (Nexus): the 32-byte directory entry followed by the file's
    // blocks with every 4 bytes reversed.
    std::vector<std::uint8_t> export_dci(const File& file) const;
    static std::optional<Incoming> parse_dci(const std::vector<std::uint8_t>& dci, std::string& error);
    bool import_dci(const std::vector<std::uint8_t>& dci, std::string& error);

    // VMS + VMI: the file's bytes, plus a 108-byte description that names
    // the file on the card, its type and date. `resource` is the VMS file
    // name without extension (at most 8 characters) the VMI points to.
    std::vector<std::uint8_t> export_vms(const File& file) const;
    std::array<std::uint8_t, kVmiSize> export_vmi(const File& file, const std::string& resource) const;
    // With no VMI the card name is made from `fallback_name` and the type
    // guessed from where the save header sits.
    static std::optional<Incoming> parse_vms(const std::vector<std::uint8_t>& vms,
                                             const std::vector<std::uint8_t>* vmi,
                                             const std::string& fallback_name, std::string& error);
    // The VMS resource a VMI points to (without extension), empty if not a VMI.
    static std::string vmi_resource(const std::vector<std::uint8_t>& vmi);

    // Copy a file from another card.
    bool copy_from(const VmuImage& other, const File& file, std::string& error,
                   bool replace = false);

    const std::array<std::uint8_t, kImageSize>& bytes() const { return m_data; }

private:
    std::uint16_t fat(int block) const;
    void set_fat(int block, std::uint16_t value);
    const std::uint8_t* block(int index) const { return m_data.data() + index * kBlockSize; }
    std::uint8_t*       block(int index)       { return m_data.data() + index * kBlockSize; }
    std::vector<int> chain(int first, int count) const;
    std::vector<int> directory_blocks() const;
    const std::uint8_t* dir_entry(const File& file) const;
    std::uint8_t*       dir_entry(const File& file);
    bool add_file(const std::uint8_t* dir_entry, const std::vector<std::uint8_t>& data,
                  std::string& error);

    std::array<std::uint8_t, kImageSize> m_data{};
};

} // namespace ds::frontend
