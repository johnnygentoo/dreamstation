// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <dreamstation/common/result.hpp>
#include <dreamstation/common/types.hpp>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace ds::frontend {

enum class TrackType : u8 {
    Audio = 0,   // CD-DA
    Data  = 4,   // Mode1 data (we treat Mode2 same here for now)
};

struct Track {
    u32                   number;
    u32                   lba_start;     // absolute LBA on the disc
    u32                   sector_size;   // 2352 raw, 2048 cooked
    TrackType             type;
    std::filesystem::path file;
    u64                   file_offset;   // bytes into file where track begins
    u64                   length_sectors;
};

// Abstract base — GDI, CDI, CHD all implement this.
class DiscImage {
public:
    virtual ~DiscImage() = default;

    [[nodiscard]] virtual std::span<const Track> tracks() const = 0;

    // Read a single 2048-byte logical (cooked) sector at the given absolute
    // LBA. Handles the 16-byte sync/header skip for raw 2352 tracks.
    [[nodiscard]] virtual common::Result<void>
        read_sector(u32 lba, std::span<u8, 2048> out) = 0;

    // Factory: sniffs extension and constructs the right subclass.
    [[nodiscard]] static common::Result<std::unique_ptr<DiscImage>>
        open(const std::filesystem::path& path);
};

} // namespace ds::frontend
