// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.hpp"
#include <dreamstation/common/log.hpp>
#include <toml++/toml.hpp>
#include <cstdlib>
#include <fstream>

namespace ds::frontend {

namespace {
std::filesystem::path xdg(const char* var, const char* fallback) {
    if (const char* v = std::getenv(var); v && *v) return v;
    if (const char* h = std::getenv("HOME"); h && *h)
        return std::filesystem::path(h) / fallback;
    return std::filesystem::temp_directory_path();
}
} // namespace

std::filesystem::path Config::config_dir() {
    return xdg("XDG_CONFIG_HOME", ".config") / "dreamstation";
}

std::filesystem::path Config::data_dir() {
    return xdg("XDG_DATA_HOME", ".local/share") / "dreamstation";
}

std::filesystem::path Config::cache_dir() {
    return xdg("XDG_CACHE_HOME", ".cache") / "dreamstation";
}

std::filesystem::path Config::covers_dir() {
    return data_dir() / "covers";
}

namespace {
std::filesystem::path ensured(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
}
} // namespace

std::filesystem::path Config::savestates_dir()   { return ensured(data_dir() / "savestates"); }
std::filesystem::path Config::screenshots_dir()  { return ensured(data_dir() / "screenshots"); }
std::filesystem::path Config::gamesettings_dir() { return ensured(data_dir() / "gamesettings"); }

Config Config::load() {
    Config cfg;
    const auto path = config_dir() / "config.toml";
    if (!std::filesystem::exists(path)) return cfg;

    try {
        const auto t = toml::parse_file(path.string());

        if (auto v = t["paths"]["bios"].value<std::string>())   cfg.paths.bios   = *v;
        if (auto v = t["paths"]["flash"].value<std::string>())  cfg.paths.flash  = *v;
        if (auto* a = t["paths"]["game_directories"].as_array())
            for (const auto& e : *a)
                if (auto v = e.value<std::string>()) cfg.paths.game_dirs.emplace_back(*v);

        if (auto* a = t["paths"]["excluded"].as_array())
            for (const auto& e : *a)
                if (auto v = e.value<std::string>()) cfg.paths.excluded.emplace_back(*v);
        if (auto v = t["library"]["view"].value<std::string>())       cfg.library.view = *v;
        if (auto v = t["library"]["sort_key"].value<std::string>())   cfg.library.sort_key = *v;
        if (auto v = t["library"]["descending"].value<bool>())        cfg.library.descending = *v;
        if (auto v = t["library"]["cover_size"].value<int64_t>())     cfg.library.cover_size = int(*v);
        if (auto* a = t["library"]["cover_urls"].as_array())
            for (const auto& e : *a)
                if (auto v = e.value<std::string>()) cfg.library.cover_urls.emplace_back(*v);

        if (auto v = t["graphics"]["internal_scale"].value<int64_t>())
            cfg.graphics.internal_scale = u32(*v);
        if (auto v = t["graphics"]["vsync"].value<bool>())      cfg.graphics.vsync = *v;

        if (auto v = t["interface"]["custom_theme"].value<bool>())
            cfg.iface.custom_theme    = *v;
        if (auto v = t["interface"]["show_toolbar"].value<bool>())
            cfg.iface.show_toolbar    = *v;
        if (auto v = t["interface"]["show_status_bar"].value<bool>())
            cfg.iface.show_status_bar = *v;
        if (auto v = t["interface"]["view_mode"].value<std::string>())
            cfg.iface.view_mode       = *v;

        const auto flag = [&t](const char* key, bool& out) {
            if (auto v = t["behavior"][key].value<bool>()) out = *v;
        };
        flag("confirm_close",           cfg.behavior.confirm_close);
        flag("save_state_on_close",     cfg.behavior.save_state_on_close);
        flag("pause_on_focus_loss",     cfg.behavior.pause_on_focus_loss);
        flag("pause_in_library",        cfg.behavior.pause_in_library);
        flag("hide_cursor_fullscreen",  cfg.behavior.hide_cursor_fullscreen);
        flag("inhibit_screensaver",     cfg.behavior.inhibit_screensaver);
        flag("double_click_fullscreen", cfg.behavior.double_click_fullscreen);
        flag("start_fullscreen",        cfg.behavior.start_fullscreen);
        flag("rumble",                  cfg.behavior.rumble);
        if (auto v = t["library"]["merge_discs"].value<bool>()) cfg.library.merge_discs = *v;
        if (auto v = t["network"]["netplay_host"].value<bool>())         cfg.network.netplay_host = *v;
        if (auto v = t["network"]["netplay_peer"].value<std::string>())  cfg.network.netplay_peer = *v;
        if (auto v = t["network"]["netplay_delay"].value<int64_t>())     cfg.network.netplay_delay = int(*v);
        if (auto v = t["network"]["dns"].value<std::string>())           cfg.network.dns = *v;
        if (auto v = t["library"]["auto_covers"].value<bool>()) cfg.library.auto_covers = *v;
        if (auto v = t["library"]["watch_dirs"].value<bool>())  cfg.library.watch_dirs = *v;
        if (auto v = t["library"]["box_covers"].value<bool>())  cfg.library.box_covers = *v;
        if (auto v = t["osd"]["messages"].value<bool>())          cfg.osd.messages = *v;
        if (auto v = t["osd"]["position"].value<std::string>())   cfg.osd.position = *v;
        if (auto v = t["osd"]["scale"].value<int64_t>())          cfg.osd.scale = int(*v);
        if (auto v = t["osd"]["show_fps"].value<bool>())          cfg.osd.show_fps = *v;
        if (auto v = t["osd"]["show_speed"].value<bool>())        cfg.osd.show_speed = *v;
        if (auto v = t["osd"]["show_resolution"].value<bool>())   cfg.osd.show_resolution = *v;
        if (auto v = t["osd"]["vmu_screen"].value<bool>())        cfg.osd.vmu_screen = *v;
        if (auto v = t["interface"]["vmu_window"].value<bool>())  cfg.iface.vmu_window = *v;
        if (auto v = t["osd"]["show_frame_times"].value<bool>())  cfg.osd.show_frame_times = *v;
        if (auto v = t["display"]["aspect"].value<std::string>()) cfg.display.aspect = *v;
        if (auto v = t["display"]["filter"].value<std::string>()) cfg.display.filter = *v;
        if (auto v = t["display"]["integer"].value<bool>())       cfg.display.integer = *v;
        if (auto v = t["display"]["speed"].value<int64_t>())      cfg.display.speed = int(*v);
        if (auto v = t["audio"]["volume"].value<int64_t>())     cfg.audio.volume = int(*v);
        if (auto v = t["audio"]["muted"].value<bool>())         cfg.audio.muted = *v;
        if (auto v = t["audio"]["device"].value<std::string>()) cfg.audio.device = *v;
        if (auto v = t["audio"]["buffer_ms"].value<int64_t>())  cfg.audio.buffer_ms = int(*v);

        if (const auto* pp = t["postprocessing"].as_table()) {
            if (auto v = (*pp)["enabled"].value<bool>()) cfg.post_enabled = *v;
            if (auto v = (*pp)["native_input"].value<bool>()) cfg.post_native_input = *v;
            if (const auto* stages = (*pp)["stage"].as_array()) {
                for (const auto& node : *stages) {
                    const auto* st = node.as_table();
                    if (!st) continue;
                    PostStage stage;
                    if (auto v = (*st)["shader"].value<std::string>()) stage.shader = *v;
                    if (auto v = (*st)["enabled"].value<bool>())       stage.enabled = *v;
                    if (const auto* opts = (*st)["options"].as_table())
                        for (const auto& [k, v] : *opts)
                            if (auto d = v.value<double>())
                                stage.options[std::string(k.str())] = static_cast<float>(*d);
                    if (!stage.shader.empty())
                        cfg.post_chain.push_back(std::move(stage));
                }
            }
        } else {
            // First run with the chain: carry over the old single-shader choice.
            const std::string old = t["graphics"]["shader"].value_or(std::string());
            if (old == "crt")        cfg.post_chain.push_back({"dreamstation/crt", true, {}});
            else if (old == "sharp") cfg.post_chain.push_back({"dreamstation/sharp-bilinear", true, {}});
        }
        const auto string_map = [&t](const char* table, std::map<std::string, std::string>& out) {
            if (auto* tbl = t[table].as_table())
                for (const auto& [k, v] : *tbl)
                    if (auto sv = v.value<std::string>())
                        out[std::string(k.str())] = *sv;
        };
        string_map("hotkeys", cfg.hotkeys);
        string_map("hotkey_pads", cfg.hotkey_pads);
        string_map("engine",  cfg.engine);
        string_map("bindings", cfg.bindings);

        if (auto v = t["window"]["width"].value<int64_t>())  cfg.window.width     = int(*v);
        if (auto v = t["window"]["height"].value<int64_t>()) cfg.window.height    = int(*v);
        if (auto v = t["window"]["maximized"].value<bool>()) cfg.window.maximized = *v;

    } catch (const toml::parse_error& e) {
        ds::common::log()->warn(
            "Config: malformed config.toml ({}); resetting to defaults "
            "and rewriting the file", e.description());
        cfg.save();  // overwrite with clean defaults so next launch succeeds
    }
    return cfg;
}

void Config::save() const {
    std::error_code ec;
    std::filesystem::create_directories(config_dir(), ec);
    const auto path = config_dir() / "config.toml";

    // Build the TOML document via toml++'s typed API so all strings are
    // properly escaped (handles paths with backslashes, quotes, etc.).
    auto root = toml::table{};

    auto paths_tbl = toml::table{};
    if (!paths.bios.empty())
        paths_tbl.emplace("bios",  paths.bios.string());
    if (!paths.flash.empty())
        paths_tbl.emplace("flash", paths.flash.string());
    auto dirs_arr = toml::array{};
    for (const auto& d : paths.game_dirs)
        dirs_arr.push_back(d.string());
    paths_tbl.emplace("game_directories", std::move(dirs_arr));
    auto excl_arr = toml::array{};
    for (const auto& d : paths.excluded)
        excl_arr.push_back(d.string());
    paths_tbl.emplace("excluded", std::move(excl_arr));
    root.emplace("paths", std::move(paths_tbl));

    root.emplace("graphics", toml::table{
        {"internal_scale", static_cast<int64_t>(graphics.internal_scale)},
        {"vsync",          graphics.vsync},
    });

    auto stage_arr = toml::array{};
    for (const auto& stage : post_chain) {
        auto opts = toml::table{};
        for (const auto& [k, v] : stage.options)
            opts.emplace(k, static_cast<double>(v));
        stage_arr.push_back(toml::table{
            {"shader", stage.shader}, {"enabled", stage.enabled}, {"options", std::move(opts)}});
    }
    root.emplace("postprocessing", toml::table{
        {"enabled", post_enabled}, {"native_input", post_native_input},
        {"stage", std::move(stage_arr)}});

    auto urls_arr = toml::array{};
    for (const auto& u : library.cover_urls)
        urls_arr.push_back(u);
    root.emplace("network", toml::table{
        {"netplay_host",  network.netplay_host},
        {"netplay_peer",  network.netplay_peer},
        {"netplay_delay", static_cast<int64_t>(network.netplay_delay)},
        {"dns",           network.dns},
    });
    root.emplace("library", toml::table{
        {"view",       library.view},
        {"sort_key",   library.sort_key},
        {"descending", library.descending},
        {"cover_size", static_cast<int64_t>(library.cover_size)},
        {"merge_discs", library.merge_discs},
        {"auto_covers", library.auto_covers},
        {"watch_dirs",  library.watch_dirs},
        {"box_covers",  library.box_covers},
        {"cover_urls", std::move(urls_arr)},
    });

    root.emplace("behavior", toml::table{
        {"confirm_close",           behavior.confirm_close},
        {"save_state_on_close",     behavior.save_state_on_close},
        {"pause_on_focus_loss",     behavior.pause_on_focus_loss},
        {"pause_in_library",        behavior.pause_in_library},
        {"hide_cursor_fullscreen",  behavior.hide_cursor_fullscreen},
        {"inhibit_screensaver",     behavior.inhibit_screensaver},
        {"double_click_fullscreen", behavior.double_click_fullscreen},
        {"start_fullscreen",        behavior.start_fullscreen},
        {"rumble",                  behavior.rumble},
    });

    root.emplace("osd", toml::table{
        {"messages",         osd.messages},
        {"position",         osd.position},
        {"scale",            static_cast<int64_t>(osd.scale)},
        {"show_fps",         osd.show_fps},
        {"show_speed",       osd.show_speed},
        {"show_resolution",  osd.show_resolution},
        {"show_frame_times", osd.show_frame_times},
        {"vmu_screen",       osd.vmu_screen},
    });

    root.emplace("display", toml::table{
        {"aspect",  display.aspect},
        {"filter",  display.filter},
        {"integer", display.integer},
        {"speed",   static_cast<int64_t>(display.speed)},
    });

    root.emplace("audio", toml::table{
        {"volume",    static_cast<int64_t>(audio.volume)},
        {"muted",     audio.muted},
        {"device",    audio.device},
        {"buffer_ms", static_cast<int64_t>(audio.buffer_ms)},
    });

    const auto emit_map = [&root](const char* name,
                                  const std::map<std::string, std::string>& values) {
        auto tbl = toml::table{};
        for (const auto& [k, v] : values)
            tbl.emplace(k, v);
        root.emplace(name, std::move(tbl));
    };
    emit_map("hotkeys", hotkeys);
    emit_map("hotkey_pads", hotkey_pads);
    emit_map("engine",  engine);
    emit_map("bindings", bindings);

    root.emplace("interface", toml::table{
        {"custom_theme",    iface.custom_theme},
        {"show_toolbar",    iface.show_toolbar},
        {"show_status_bar", iface.show_status_bar},
        {"view_mode",       iface.view_mode},
        {"vmu_window",      iface.vmu_window},
    });

    root.emplace("window", toml::table{
        {"width",     static_cast<int64_t>(window.width)},
        {"height",    static_cast<int64_t>(window.height)},
        {"maximized", window.maximized},
    });

    std::ofstream f(path);
    if (!f) { ds::common::log()->error("Config: cannot write {}", path.string()); return; }
    f << root;
}

GameSettings GameSettings::load(const std::string& game_id) {
    GameSettings gs;
    const auto path = Config::gamesettings_dir() / (game_id + ".toml");
    if (!std::filesystem::exists(path))
        return gs;
    try {
        const auto t = toml::parse_file(path.string());
        if (auto* tbl = t["engine"].as_table())
            for (const auto& [k, v] : *tbl)
                if (auto sv = v.value<std::string>())
                    gs.engine[std::string(k.str())] = *sv;
    } catch (const toml::parse_error& e) {
        ds::common::log()->warn("GameSettings: {} is malformed: {}", path.string(), e.description());
    }
    return gs;
}

void GameSettings::save(const std::string& game_id) const {
    const auto path = Config::gamesettings_dir() / (game_id + ".toml");
    if (engine.empty()) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return;
    }
    auto tbl = toml::table{};
    for (const auto& [k, v] : engine)
        tbl.emplace(k, v);
    auto root = toml::table{};
    root.emplace("engine", std::move(tbl));
    std::ofstream f(path);
    if (f) f << root;
}

} // namespace ds::frontend
