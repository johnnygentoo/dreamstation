// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <expected>
#include <string>
#include <utility>

namespace ds::common {

struct Error {
    std::string message;
    int code = 0;
};

// Thin wrapper around std::expected that also accepts Error directly,
// so callers can write `return common::Error{"msg"};` without wrapping
// in std::unexpected manually.
template<typename T>
class Result : public std::expected<T, Error> {
public:
    using std::expected<T, Error>::expected;
    Result(Error e)
        : std::expected<T, Error>(std::unexpected(std::move(e))) {}
};

template<>
class Result<void> : public std::expected<void, Error> {
public:
    using std::expected<void, Error>::expected;
    Result(Error e)
        : std::expected<void, Error>(std::unexpected(std::move(e))) {}
};

} // namespace ds::common
