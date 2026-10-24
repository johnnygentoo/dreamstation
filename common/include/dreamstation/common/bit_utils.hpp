// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <dreamstation/common/types.hpp>
#include <bit>

namespace ds::common {

template<ds::u32 N>
constexpr ds::s32 sign_extend(ds::u32 v) {
    static_assert(N > 0 && N < 32);
    constexpr ds::u32 sign_bit = 1u << (N - 1);
    return (v & sign_bit) ? static_cast<ds::s32>(v | (~0u << N))
                          : static_cast<ds::s32>(v & (sign_bit - 1u));
}

constexpr ds::u32 rotate_left (ds::u32 v, int n) { return std::rotl(v, n); }
constexpr ds::u32 rotate_right(ds::u32 v, int n) { return std::rotr(v, n); }

} // namespace ds::common
