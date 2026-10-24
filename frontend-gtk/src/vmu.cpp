// SPDX-License-Identifier: GPL-3.0-or-later
#include "vmu.hpp"

#include <glib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <fstream>

namespace ds::frontend {

namespace {

constexpr int kRootBlock = 255;
constexpr int kFatBlock  = 254;
constexpr int kDirBlock  = 253;
constexpr int kDirBlocks = 13;
constexpr std::uint16_t kFatFree = 0xFFFC;
constexpr std::uint16_t kFatLast = 0xFFFA;
constexpr std::size_t   kDirEntry = 32;
constexpr std::uint8_t  kTypeData = 0x33;
constexpr std::uint8_t  kTypeGame = 0xCC;

std::uint16_t rd16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
void wr16(std::uint8_t* p, std::uint16_t v) { p[0] = v & 0xFF; p[1] = static_cast<std::uint8_t>(v >> 8); }

std::uint8_t to_bcd(int v) { return static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10)); }
int from_bcd(std::uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

// Card text is Shift-JIS (ASCII for most western saves), space padded.
std::string card_text(const std::uint8_t* p, std::size_t len) {
    while (len > 0 && (p[len - 1] == 0 || p[len - 1] == ' ')) --len;
    if (len == 0)
        return {};
    const char* s = reinterpret_cast<const char*>(p);
    if (g_utf8_validate(s, static_cast<gssize>(len), nullptr))
        return std::string(s, len);
    gsize written = 0;
    if (gchar* u = g_convert(s, static_cast<gssize>(len), "UTF-8", "SHIFT-JIS", nullptr,
                             &written, nullptr)) {
        std::string out(u, written);
        g_free(u);
        return out;
    }
    std::string out;
    for (std::size_t i = 0; i < len; ++i)
        out += p[i] < 0x80 ? static_cast<char>(p[i]) : '?';
    return out;
}

// ARGB4444 (the card's colour format) to 8-bit RGBA.
void put_argb4444(std::uint8_t* rgba, std::uint16_t c) {
    rgba[0] = static_cast<std::uint8_t>(((c >> 8) & 0xF) * 17);
    rgba[1] = static_cast<std::uint8_t>(((c >> 4) & 0xF) * 17);
    rgba[2] = static_cast<std::uint8_t>((c & 0xF) * 17);
    rgba[3] = static_cast<std::uint8_t>(((c >> 12) & 0xF) * 17);
}

// Card text fields: padded with `pad` to `len` bytes, truncated if longer.
void put_text(std::uint8_t* p, std::size_t len, const std::string& text, std::uint8_t pad) {
    std::memset(p, pad, len);
    std::memcpy(p, text.data(), std::min(len, text.size()));
}

std::vector<std::uint8_t> swap32(const std::uint8_t* p, std::size_t size) {
    std::vector<std::uint8_t> out(size);
    for (std::size_t i = 0; i + 3 < size; i += 4) {
        out[i]     = p[i + 3];
        out[i + 1] = p[i + 2];
        out[i + 2] = p[i + 1];
        out[i + 3] = p[i];
    }
    return out;
}

bool printable(const std::uint8_t* p, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i)
        if (p[i] < 0x20 || p[i] == 0x7F)
            return false;
    return true;
}

} // namespace

const std::vector<std::uint8_t>& VmuImage::File::icon() const {
    static const std::vector<std::uint8_t> none;
    return icons.empty() ? none : icons.front();
}

std::string VmuImage::Incoming::name() const { return card_text(entry.data() + 4, 12); }

// ── Low level ─────────────────────────────────────────────────────────────

std::uint16_t VmuImage::fat(int b) const { return rd16(block(kFatBlock) + b * 2); }
void VmuImage::set_fat(int b, std::uint16_t v) { wr16(block(kFatBlock) + b * 2, v); }

std::vector<int> VmuImage::chain(int first, int count) const {
    std::vector<int> out;
    int b = first;
    // `count` bounds the walk so a damaged FAT cannot loop forever.
    while (b >= 0 && b < static_cast<int>(kBlocks) && static_cast<int>(out.size()) < count) {
        out.push_back(b);
        const std::uint16_t next = fat(b);
        if (next >= kFatLast)
            break;
        b = next;
    }
    return out;
}

std::vector<int> VmuImage::directory_blocks() const {
    return chain(kDirBlock, kDirBlocks);
}

// ── Whole image ───────────────────────────────────────────────────────────

VmuImage VmuImage::formatted() {
    VmuImage v;
    std::uint8_t* root = v.block(kRootBlock);
    std::memset(root, 0x55, 16);                 // format signature
    root[0x10] = 0x01;                           // custom colour flag
    root[0x11] = 0xFF; root[0x12] = 0xFF; root[0x13] = 0xFF; root[0x14] = 0xFF;
    const std::time_t now = std::time(nullptr);
    const std::tm tm = *std::localtime(&now);
    root[0x30] = to_bcd((tm.tm_year + 1900) / 100);
    root[0x31] = to_bcd(tm.tm_year % 100);
    root[0x32] = to_bcd(tm.tm_mon + 1);
    root[0x33] = to_bcd(tm.tm_mday);
    root[0x34] = to_bcd(tm.tm_hour);
    root[0x35] = to_bcd(tm.tm_min);
    root[0x36] = to_bcd(tm.tm_sec);
    root[0x37] = to_bcd(tm.tm_wday);
    wr16(root + 0x40, 255);                      // total blocks - 1
    wr16(root + 0x46, kFatBlock);
    wr16(root + 0x48, 1);
    wr16(root + 0x4A, kDirBlock);
    wr16(root + 0x4C, kDirBlocks);
    wr16(root + 0x4E, 0);                        // icon shape
    wr16(root + 0x50, kUserBlocks);

    for (int b = 0; b < static_cast<int>(kBlocks); ++b)
        v.set_fat(b, kFatFree);
    v.set_fat(kRootBlock, kFatLast);
    v.set_fat(kFatBlock, kFatLast);
    for (int i = 0; i < kDirBlocks; ++i) {
        const int b = kDirBlock - i;
        v.set_fat(b, i + 1 < kDirBlocks ? static_cast<std::uint16_t>(b - 1) : kFatLast);
    }
    return v;
}

VmuImage::CardFormat VmuImage::format_for(const std::filesystem::path& file) {
    auto ext = file.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".dcm" ? CardFormat::Dcm : CardFormat::Raw;
}

std::optional<VmuImage> VmuImage::load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    VmuImage v;
    if (!in.read(reinterpret_cast<char*>(v.m_data.data()), static_cast<std::streamsize>(kImageSize)))
        return std::nullopt;
    if (format_for(file) == CardFormat::Dcm) {
        const auto plain = swap32(v.m_data.data(), kImageSize);
        std::copy(plain.begin(), plain.end(), v.m_data.begin());
    }
    return v;
}

VmuImage VmuImage::from_bytes(const std::array<std::uint8_t, kImageSize>& bytes) {
    VmuImage v;
    v.m_data = bytes;
    return v;
}

bool VmuImage::save(const std::filesystem::path& file) const {
    return save(file, format_for(file));
}

bool VmuImage::save(const std::filesystem::path& file, CardFormat format) const {
    // Write beside the target and rename, so a failed write cannot leave a
    // truncated card behind.
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (format == CardFormat::Dcm) {
            const auto swapped = swap32(m_data.data(), kImageSize);
            out.write(reinterpret_cast<const char*>(swapped.data()), static_cast<std::streamsize>(kImageSize));
        } else {
            out.write(reinterpret_cast<const char*>(m_data.data()), static_cast<std::streamsize>(kImageSize));
        }
        if (!out.good())
            return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

bool VmuImage::valid() const {
    const std::uint8_t* root = block(kRootBlock);
    for (int i = 0; i < 16; ++i)
        if (root[i] != 0x55)
            return false;
    return rd16(root + 0x46) == kFatBlock && rd16(root + 0x4A) == kDirBlock;
}

int VmuImage::free_blocks() const {
    int n = 0;
    for (int b = 0; b < kUserBlocks; ++b)
        n += fat(b) == kFatFree;
    return n;
}

int VmuImage::free_game_blocks() const {
    int n = 0;
    while (n < kUserBlocks && fat(n) == kFatFree)
        ++n;
    return n;
}

// ── Files ─────────────────────────────────────────────────────────────────

std::vector<VmuImage::File> VmuImage::files() const {
    std::vector<File> out;
    int index = 0;
    for (int dir : directory_blocks()) {
        for (std::size_t i = 0; i < kBlockSize / kDirEntry; ++i, ++index) {
            const std::uint8_t* e = block(dir) + i * kDirEntry;
            if (e[0] != kTypeData && e[0] != kTypeGame)
                continue;
            File f;
            f.dir_index      = index;
            f.game           = e[0] == kTypeGame;
            f.copy_protected = e[1] == 0xFF;
            f.first_block    = rd16(e + 2);
            f.name           = card_text(e + 4, 12);
            f.blocks         = rd16(e + 0x18);
            f.header_block   = rd16(e + 0x1A);
            char ts[32];
            std::snprintf(ts, sizeof(ts), "%02d%02d-%02d-%02d %02d:%02d", from_bcd(e[0x10]),
                          from_bcd(e[0x11]), from_bcd(e[0x12]), from_bcd(e[0x13]),
                          from_bcd(e[0x14]), from_bcd(e[0x15]));
            f.timestamp = ts;

            // The save header: descriptions, palette, icon frames, eyecatch.
            const auto blocks = chain(f.first_block, f.blocks);
            std::vector<std::uint8_t> head;
            for (std::size_t b = static_cast<std::size_t>(std::max(0, f.header_block));
                 b < blocks.size(); ++b)
                head.insert(head.end(), block(blocks[b]), block(blocks[b]) + kBlockSize);
            if (head.size() >= 0x80) {
                f.description      = card_text(head.data(), 16);
                f.long_description = card_text(head.data() + 0x10, 32);
                f.app_id           = card_text(head.data() + 0x30, 16);
                const int icons = rd16(head.data() + 0x40);
                f.icon_speed    = rd16(head.data() + 0x42);
                const std::uint8_t* palette = head.data() + 0x60;
                for (int n = 0; n < std::min(icons, 3); ++n) {
                    const std::size_t at = 0x80 + static_cast<std::size_t>(n) * 0x200;
                    if (head.size() < at + 0x200)
                        break;
                    std::vector<std::uint8_t> frame(32 * 32 * 4);
                    for (std::size_t p = 0; p < 32 * 32; ++p) {
                        const std::uint8_t byte = head[at + p / 2];
                        const int idx = (p & 1) ? (byte & 0x0F) : (byte >> 4);
                        put_argb4444(frame.data() + p * 4, rd16(palette + idx * 2));
                    }
                    f.icons.push_back(std::move(frame));
                }
                // Eyecatch: 72x56, after the icons, in one of three colour modes.
                constexpr std::size_t kPixels = File::kEyecatchWidth * File::kEyecatchHeight;
                const std::size_t at = 0x80 + static_cast<std::size_t>(std::max(icons, 0)) * 0x200;
                const int mode = rd16(head.data() + 0x44);
                const std::size_t need = mode == 1 ? kPixels * 2 : mode == 2 ? 512 + kPixels
                                       : mode == 3 ? 32 + kPixels / 2 : 0;
                if (need && icons <= 3 && head.size() >= at + need) {
                    const std::uint8_t* pic = head.data() + at;
                    f.eyecatch.resize(kPixels * 4);
                    for (std::size_t p = 0; p < kPixels; ++p) {
                        std::uint16_t c = 0;
                        if (mode == 1)
                            c = rd16(pic + p * 2);
                        else if (mode == 2)
                            c = rd16(pic + pic[512 + p] * 2);
                        else
                            c = rd16(pic + (((p & 1) ? pic[32 + p / 2] & 0x0F : pic[32 + p / 2] >> 4) * 2));
                        put_argb4444(f.eyecatch.data() + p * 4, c);
                    }
                }
            }
            out.push_back(std::move(f));
        }
    }
    return out;
}

std::optional<VmuImage::File> VmuImage::find(const std::string& name) const {
    for (auto& f : files())
        if (f.name == name)
            return f;
    return std::nullopt;
}

const std::uint8_t* VmuImage::dir_entry(const File& file) const {
    const auto dirs = directory_blocks();
    const std::size_t per = kBlockSize / kDirEntry;
    const std::size_t di = static_cast<std::size_t>(file.dir_index);
    if (di / per >= dirs.size())
        return nullptr;
    return block(dirs[di / per]) + (di % per) * kDirEntry;
}

std::uint8_t* VmuImage::dir_entry(const File& file) {
    return const_cast<std::uint8_t*>(static_cast<const VmuImage*>(this)->dir_entry(file));
}

std::vector<std::uint8_t> VmuImage::read(const File& file) const {
    std::vector<std::uint8_t> out;
    for (int b : chain(file.first_block, file.blocks))
        out.insert(out.end(), block(b), block(b) + kBlockSize);
    return out;
}

bool VmuImage::remove(const File& file) {
    std::uint8_t* e = dir_entry(file);
    if (!e || (e[0] != kTypeData && e[0] != kTypeGame))
        return false;
    for (int b : chain(rd16(e + 2), rd16(e + 0x18)))
        set_fat(b, kFatFree);
    std::memset(e, 0, kDirEntry);
    return true;
}

bool VmuImage::add_file(const std::uint8_t* dir_entry, const std::vector<std::uint8_t>& data,
                        std::string& error) {
    const int blocks = static_cast<int>((data.size() + kBlockSize - 1) / kBlockSize);
    if (blocks == 0) {
        error = "The file is empty";
        return false;
    }
    const std::string name = card_text(dir_entry + 4, 12);
    for (const File& f : files())
        if (f.name == name) {
            error = "A file named " + name + " is already on this VMU";
            return false;
        }
    if (blocks > free_blocks()) {
        error = "Not enough free blocks (" + std::to_string(blocks) + " needed, " +
                std::to_string(free_blocks()) + " free)";
        return false;
    }
    const bool game = dir_entry[0] == kTypeGame;

    // Save data is allocated from the top of the user area downward; a
    // mini-game must sit in one unbroken run starting at block 0.
    std::vector<int> alloc;
    if (game) {
        for (int b = 0; b < blocks; ++b) {
            if (fat(b) != kFatFree) {
                error = "A VMU game needs the first " + std::to_string(blocks) +
                        " blocks free and unbroken";
                return false;
            }
            alloc.push_back(b);
        }
    } else {
        for (int b = kUserBlocks - 1; b >= 0 && static_cast<int>(alloc.size()) < blocks; --b)
            if (fat(b) == kFatFree)
                alloc.push_back(b);
    }

    // A free directory slot.
    std::uint8_t* slot = nullptr;
    for (int dir : directory_blocks()) {
        for (std::size_t i = 0; i < kBlockSize / kDirEntry && !slot; ++i) {
            std::uint8_t* e = block(dir) + i * kDirEntry;
            if (e[0] != kTypeData && e[0] != kTypeGame)
                slot = e;
        }
        if (slot) break;
    }
    if (!slot) {
        error = "The VMU's directory is full";
        return false;
    }

    for (std::size_t i = 0; i < alloc.size(); ++i) {
        std::uint8_t* dst = block(alloc[i]);
        std::memset(dst, 0, kBlockSize);
        const std::size_t off = i * kBlockSize;
        std::memcpy(dst, data.data() + off, std::min(kBlockSize, data.size() - off));
        set_fat(alloc[i], i + 1 < alloc.size() ? static_cast<std::uint16_t>(alloc[i + 1]) : kFatLast);
    }
    std::memcpy(slot, dir_entry, kDirEntry);
    wr16(slot + 2, static_cast<std::uint16_t>(alloc.front()));
    wr16(slot + 0x18, static_cast<std::uint16_t>(blocks));
    return true;
}

bool VmuImage::set_copy_protected(const File& file, bool on) {
    std::uint8_t* e = dir_entry(file);
    if (!e || (e[0] != kTypeData && e[0] != kTypeGame))
        return false;
    e[1] = on ? 0xFF : 0x00;
    return true;
}

VmuImage::Incoming VmuImage::take(const File& file) const {
    Incoming in;
    if (const std::uint8_t* e = dir_entry(file))
        std::memcpy(in.entry.data(), e, kDirEntry);
    in.data = read(file);
    return in;
}

bool VmuImage::add(const Incoming& file, bool replace, std::string& error) {
    if (file.entry[0] != kTypeData && file.entry[0] != kTypeGame) {
        error = "Not a VMU file";
        return false;
    }
    const auto existing = find(file.name());
    if (existing && !replace) {
        error = "A file named " + file.name() + " is already on this VMU";
        return false;
    }
    // Work on a copy so a replacement that does not fit leaves the card as it was.
    VmuImage next = *this;
    if (existing)
        next.remove(*existing);
    if (file.game()) {
        // Only one mini-game fits on a card: it owns the blocks from 0 up.
        for (const File& f : next.files())
            if (f.game) {
                error = "This VMU already holds a mini-game (" + f.name + "); a card has room "
                        "for only one";
                return false;
            }
    }
    if (!next.add_file(file.entry.data(), file.data, error))
        return false;
    m_data = next.m_data;
    return true;
}

// ── Exchange ──────────────────────────────────────────────────────────────

std::vector<std::uint8_t> VmuImage::export_dci(const File& file) const {
    std::vector<std::uint8_t> out;
    const std::uint8_t* e = dir_entry(file);
    if (!e)
        return out;
    out.assign(e, e + kDirEntry);
    const auto data = read(file);
    const auto swapped = swap32(data.data(), data.size());
    out.insert(out.end(), swapped.begin(), swapped.end());
    return out;
}

std::optional<VmuImage::Incoming> VmuImage::parse_dci(const std::vector<std::uint8_t>& dci,
                                                      std::string& error) {
    if (dci.size() <= kDirEntry || (dci.size() - kDirEntry) % kBlockSize != 0 ||
        (dci[0] != kTypeData && dci[0] != kTypeGame)) {
        error = "Not a DCI save file";
        return std::nullopt;
    }
    Incoming in;
    std::memcpy(in.entry.data(), dci.data(), kDirEntry);
    in.data = swap32(dci.data() + kDirEntry, dci.size() - kDirEntry);
    return in;
}

bool VmuImage::import_dci(const std::vector<std::uint8_t>& dci, std::string& error) {
    const auto in = parse_dci(dci, error);
    return in && add(*in, false, error);
}

std::vector<std::uint8_t> VmuImage::export_vms(const File& file) const {
    return read(file);
}

std::array<std::uint8_t, VmuImage::kVmiSize> VmuImage::export_vmi(const File& file,
                                                                  const std::string& resource) const {
    std::array<std::uint8_t, kVmiSize> vmi{};
    const std::uint8_t* e = dir_entry(file);
    if (!e)
        return vmi;
    put_text(vmi.data() + 0x04, 32, file.long_description.empty() ? file.name : file.long_description, 0);
    put_text(vmi.data() + 0x24, 32, "DreamStation", 0);
    // Creation date from the directory entry (BCD, weekday 0 = Monday).
    wr16(vmi.data() + 0x44, static_cast<std::uint16_t>(from_bcd(e[0x10]) * 100 + from_bcd(e[0x11])));
    for (int i = 0; i < 5; ++i)
        vmi[0x46 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(from_bcd(e[0x12 + i]));
    vmi[0x4B] = static_cast<std::uint8_t>((from_bcd(e[0x17]) + 1) % 7);   // 0 = Sunday here
    wr16(vmi.data() + 0x4C, 0);
    wr16(vmi.data() + 0x4E, 1);
    put_text(vmi.data() + 0x50, 8, resource, 0);
    // The card pads names with spaces, a VMI with NULs.
    put_text(vmi.data() + 0x58, 12, card_text(e + 4, 12), 0);
    wr16(vmi.data() + 0x64, static_cast<std::uint16_t>((file.game ? 2 : 0) | (file.copy_protected ? 1 : 0)));
    const std::uint32_t size = static_cast<std::uint32_t>(file.blocks) * kBlockSize;
    for (int i = 0; i < 4; ++i)
        vmi[0x68 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(size >> (8 * i));
    static const char kSega[] = "SEGA";
    for (int i = 0; i < 4; ++i)
        vmi[static_cast<std::size_t>(i)] = vmi[0x50 + static_cast<std::size_t>(i)] & static_cast<std::uint8_t>(kSega[i]);
    return vmi;
}

std::string VmuImage::vmi_resource(const std::vector<std::uint8_t>& vmi) {
    if (vmi.size() != kVmiSize)
        return {};
    std::size_t len = 0;
    while (len < 8 && vmi[0x50 + len] != 0)
        ++len;
    return std::string(reinterpret_cast<const char*>(vmi.data() + 0x50), len);
}

std::optional<VmuImage::Incoming> VmuImage::parse_vms(const std::vector<std::uint8_t>& vms,
                                                      const std::vector<std::uint8_t>* vmi,
                                                      const std::string& fallback_name,
                                                      std::string& error) {
    if (vms.empty() || vms.size() > static_cast<std::size_t>(kUserBlocks) * kBlockSize) {
        error = "Not a VMS file (wrong size)";
        return std::nullopt;
    }
    Incoming in;
    in.data = vms;
    in.data.resize(static_cast<std::size_t>(in.blocks()) * kBlockSize, 0);
    std::uint8_t* e = in.entry.data();

    bool game = false, protect = false;
    if (vmi) {
        if (vmi->size() != kVmiSize) {
            error = "The VMI file is not 108 bytes";
            return std::nullopt;
        }
        const std::uint16_t mode = rd16(vmi->data() + 0x64);
        game    = mode & 2;
        protect = mode & 1;
        std::memcpy(e + 4, vmi->data() + 0x58, 12);
        // Names are padded with NULs in a VMI and with spaces on the card.
        for (int i = 4; i < 16; ++i)
            if (e[i] == 0) e[i] = ' ';
        const int year = rd16(vmi->data() + 0x44);
        e[0x10] = to_bcd(year / 100);
        e[0x11] = to_bcd(year % 100);
        for (int i = 0; i < 5; ++i)
            e[0x12 + i] = to_bcd((*vmi)[0x46 + static_cast<std::size_t>(i)]);
        e[0x17] = to_bcd(((*vmi)[0x4B] + 6) % 7);   // VMI: 0 = Sunday; card: 0 = Monday
    } else {
        // No VMI: a game keeps its header in the second block, after code;
        // save data starts with it.
        game = in.data.size() >= 0x210 && !printable(in.data.data(), 16) &&
               printable(in.data.data() + 0x200, 16);
        std::string name;
        for (char c : fallback_name)
            name += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        put_text(e + 4, 12, name, ' ');
        const std::time_t now = std::time(nullptr);
        const std::tm tm = *std::localtime(&now);
        e[0x10] = to_bcd((tm.tm_year + 1900) / 100);
        e[0x11] = to_bcd(tm.tm_year % 100);
        e[0x12] = to_bcd(tm.tm_mon + 1);
        e[0x13] = to_bcd(tm.tm_mday);
        e[0x14] = to_bcd(tm.tm_hour);
        e[0x15] = to_bcd(tm.tm_min);
        e[0x16] = to_bcd(tm.tm_sec);
        e[0x17] = to_bcd((tm.tm_wday + 6) % 7);
    }
    e[0] = game ? kTypeGame : kTypeData;
    e[1] = protect ? 0xFF : 0x00;
    wr16(e + 0x18, static_cast<std::uint16_t>(in.blocks()));
    wr16(e + 0x1A, game ? 1 : 0);
    return in;
}

bool VmuImage::copy_from(const VmuImage& other, const File& file, std::string& error,
                         bool replace) {
    const Incoming in = other.take(file);
    if (in.data.empty()) {
        error = "Could not read the file from the source VMU";
        return false;
    }
    return add(in, replace, error);
}

} // namespace ds::frontend
