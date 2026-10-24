// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <dreamstation/common/types.hpp>
#include <filesystem>
#include <fstream>
#include <vector>

namespace ds::common {

// Read entire file into a byte buffer. Returns an empty vector on failure.
inline std::vector<ds::u8> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto sz = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<ds::u8> buf(sz);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
    return buf;
}

} // namespace ds::common
