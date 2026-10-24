// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <spdlog/spdlog.h>
#include <memory>

namespace ds::common {

// Returns the shared DreamStation logger. Initialised on first call.
std::shared_ptr<spdlog::logger> log();

} // namespace ds::common
