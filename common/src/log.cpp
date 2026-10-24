// SPDX-License-Identifier: GPL-3.0-or-later
#include <dreamstation/common/log.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <cstdlib>
#include <string_view>

namespace ds::common {

std::shared_ptr<spdlog::logger> log() {
    static const auto l = [] {
        auto logger = spdlog::stdout_color_mt("ds");
        if (const char* env = std::getenv("DS_LOG_LEVEL")) {
            std::string_view lv{env};
            if      (lv == "trace") logger->set_level(spdlog::level::trace);
            else if (lv == "debug") logger->set_level(spdlog::level::debug);
            else if (lv == "warn")  logger->set_level(spdlog::level::warn);
            else if (lv == "err")   logger->set_level(spdlog::level::err);
            // "info" is spdlog default; no-op
        }
        return logger;
    }();
    return l;
}

} // namespace ds::common
