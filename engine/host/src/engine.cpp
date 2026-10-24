// SPDX-License-Identifier: GPL-2.0-or-later
#include <dreamstation/engine/engine.hpp>

#include <libretro.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>

// VMU access in the engine fork (shell/dreamstation/vmu_access.cpp).
extern "C" bool dreamstation_vmu_screen(int bus, int slot, unsigned char* dots,
                                        unsigned long long* serial);
extern "C" bool dreamstation_vmu_present(int bus, int slot);
extern "C" const char* dreamstation_vmu_path(int bus, int slot);
extern "C" void dreamstation_vmu_hold(int bus, int slot, bool held);
extern "C" void dreamstation_vmu_release_all();
// Netplay and Internet settings (shell/dreamstation/netplay.cpp).
extern "C" void dreamstation_netplay_configure(bool enable, bool host, const char* peer, int delay);
extern "C" bool dreamstation_netplay_start();
extern "C" int  dreamstation_netplay_poll(char* message, int size);
extern "C" void dreamstation_netplay_cancel();
extern "C" const char* dreamstation_arcade_input_name(int kind, unsigned id);
extern "C" void dreamstation_set_dns(const char* server);

namespace ds::engine {
namespace {

constexpr const char* kOptionPrefix = "reicast_";

// Defaults that differ from the engine's own because of how this host
// drives it.
//
// threaded_rendering: with it on, a call to run_frame() waits up to 100 ms
// whenever the game presents no new image (every load screen), and emulation
// free-runs between calls. We run one emulated frame per display frame from
// the render callback, so the synchronous mode is the one that paces evenly
// (measured: 30 stalls of ~100 ms in 50 s with it on, 2 of ~30 ms with it off).
const std::map<std::string, std::string> kHostDefaults = {
    {"threaded_rendering", "disabled"},
    // Arcade Test and Service reach the game. The frontend only sets them
    // from inputs bound to them (F6 and 9 by default), so a stick click
    // cannot open the operator menu by accident; without this, the Test
    // button needed to leave that menu was dropped.
    {"allow_service_buttons", "enabled"},
};

struct State {
    HostCallbacks cb;
    std::string   system_dir;
    std::string   save_dir;

    std::mutex                         opt_mutex;
    std::map<std::string, std::string> opt_values;   // full key -> value
    std::vector<OptionInfo>            opt_info;
    bool                               opt_dirty = false;

    retro_disk_control_ext_callback disk{};
    bool                            disk_set = false;
    std::array<std::uint16_t, 4>    rumble_strong{};
    std::array<std::uint16_t, 4>    rumble_weak{};

    retro_hw_render_callback hw{};
    bool                     hw_set = false;
    retro_system_av_info     av{};

    std::array<PadState, 4> pads{};
    std::array<unsigned, 4> port_devices{RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD,
                                         RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD};
    bool fast_forward       = false;
    bool initialised        = false;   // host callbacks registered
    bool core_up            = false;   // between retro_init and retro_deinit
    bool loaded             = false;
    bool shutdown_requested = false;
    bool context_live       = false;
    std::optional<Engine::Netplay> netplay;   // for the next game
};

State g;

void log_line(LogLevel lvl, const char* text)
{
    if (g.cb.on_log)
        g.cb.on_log(lvl, text);
    else
        std::fprintf(stderr, "[engine] %s\n", text);
}

void RETRO_CALLCONV log_printf(enum retro_log_level level, const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::size_t n = std::strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    LogLevel lvl = LogLevel::Info;
    switch (level) {
    case RETRO_LOG_DEBUG: lvl = LogLevel::Debug; break;
    case RETRO_LOG_WARN:  lvl = LogLevel::Warn;  break;
    case RETRO_LOG_ERROR: lvl = LogLevel::Error; break;
    default: break;
    }
    log_line(lvl, buf);
}

uintptr_t RETRO_CALLCONV hw_current_framebuffer()
{
    return g.cb.current_framebuffer ? g.cb.current_framebuffer() : 0;
}

retro_proc_address_t RETRO_CALLCONV hw_get_proc_address(const char* sym)
{
    return g.cb.get_proc_address
        ? reinterpret_cast<retro_proc_address_t>(g.cb.get_proc_address(sym))
        : nullptr;
}

bool RETRO_CALLCONV rumble_set(unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
    if (port >= g.rumble_strong.size())
        return false;
    (effect == RETRO_RUMBLE_STRONG ? g.rumble_strong : g.rumble_weak)[port] = strength;
    if (g.cb.on_rumble)
        g.cb.on_rumble(port, g.rumble_strong[port], g.rumble_weak[port]);
    return true;
}

void record_options_v2(const retro_core_options_v2* opts)
{
    if (!opts || !opts->definitions)
        return;
    std::map<std::string, std::string> categories;
    if (opts->categories)
        for (const retro_core_option_v2_category* c = opts->categories; c->key; ++c)
            categories[c->key] = c->desc ? c->desc : c->key;

    std::lock_guard lock(g.opt_mutex);
    g.opt_info.clear();
    const std::size_t prefix = std::strlen(kOptionPrefix);
    for (const retro_core_option_v2_definition* d = opts->definitions; d->key; ++d) {
        OptionInfo info;
        info.key = d->key;
        if (info.key.compare(0, prefix, kOptionPrefix) == 0)
            info.key.erase(0, prefix);
        info.label = d->desc_categorized ? d->desc_categorized : (d->desc ? d->desc : d->key);
        if (d->desc && info.label.empty())
            info.label = d->desc;
        info.info          = d->info ? d->info : "";
        info.default_value = d->default_value ? d->default_value : "";
        if (const auto hd = kHostDefaults.find(info.key); hd != kHostDefaults.end())
            info.default_value = hd->second;
        if (d->category_key) {
            info.category = d->category_key;
            const auto it = categories.find(info.category);
            info.category_label = it != categories.end() ? it->second : info.category;
        }
        for (const auto& v : d->values) {
            if (!v.value)
                break;
            info.values.emplace_back(v.value);
            info.value_labels.emplace_back(v.label ? v.label : v.value);
        }
        g.opt_info.push_back(std::move(info));
    }
}

bool RETRO_CALLCONV environment(unsigned cmd, void* data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback*>(data)->log = log_printf;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *static_cast<const char**>(data) = g.system_dir.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char**>(data) = g.save_dir.c_str();
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_ROTATION:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
        g.disk     = *static_cast<const retro_disk_control_ext_callback*>(data);
        g.disk_set = true;
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: {
        // The older, smaller interface; keep it only if the extended one
        // has not been offered.
        if (!g.disk_set) {
            const auto* d = static_cast<const retro_disk_control_callback*>(data);
            g.disk = {};
            g.disk.set_eject_state     = d->set_eject_state;
            g.disk.get_eject_state     = d->get_eject_state;
            g.disk.get_image_index     = d->get_image_index;
            g.disk.set_image_index     = d->set_image_index;
            g.disk.get_num_images      = d->get_num_images;
            g.disk.replace_image_index = d->replace_image_index;
            g.disk.add_image_index     = d->add_image_index;
            g.disk_set = true;
        }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *static_cast<unsigned*>(data) = RETRO_LANGUAGE_ENGLISH;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *static_cast<unsigned*>(data) = 2;
        return true;
    case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
        *static_cast<unsigned*>(data) = 1;   // 1 = extended interface supported
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        record_options_v2(static_cast<const retro_core_options_v2*>(data));
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
        record_options_v2(static_cast<const retro_core_options_v2_intl*>(data)->us);
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto* var = static_cast<retro_variable*>(data);
        if (!var || !var->key)
            return false;
        std::lock_guard lock(g.opt_mutex);
        auto it = g.opt_values.find(var->key);
        if (it == g.opt_values.end()) {
            // Not set by the user: our default if we have one, else the engine's.
            const std::string key = var->key;
            const std::size_t prefix = std::strlen(kOptionPrefix);
            if (key.compare(0, prefix, kOptionPrefix) == 0) {
                const auto hd = kHostDefaults.find(key.substr(prefix));
                if (hd != kHostDefaults.end()) {
                    var->value = hd->second.c_str();
                    return true;
                }
            }
            var->value = nullptr;
            return false;
        }
        var->value = it->second.c_str();
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
        std::lock_guard lock(g.opt_mutex);
        *static_cast<bool*>(data) = g.opt_dirty;
        g.opt_dirty = false;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
        *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_OPENGL_CORE;
        return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
        auto* hw = static_cast<retro_hw_render_callback*>(data);
        if (hw->context_type != RETRO_HW_CONTEXT_OPENGL_CORE &&
            hw->context_type != RETRO_HW_CONTEXT_OPENGL)
            return false;
        hw->get_current_framebuffer = hw_current_framebuffer;
        hw->get_proc_address        = hw_get_proc_address;
        g.hw     = *hw;
        g.hw_set = true;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        g.av = *static_cast<const retro_system_av_info*>(data);
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        g.av.geometry = *static_cast<const retro_game_geometry*>(data);
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        const auto* msg = static_cast<const retro_message*>(data);
        if (msg && msg->msg && g.cb.on_message)
            g.cb.on_message(msg->msg);
        return true;
    }
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        static_cast<retro_rumble_interface*>(data)->set_rumble_state = rumble_set;
        return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
        *static_cast<bool*>(data) = g.fast_forward;
        return true;
    case RETRO_ENVIRONMENT_SHUTDOWN:
        g.shutdown_requested = true;
        return true;
    default:
        return false;
    }
}

void RETRO_CALLCONV video_refresh(const void* data, unsigned w, unsigned h, size_t)
{
    if (g.cb.on_frame)
        g.cb.on_frame(w, h, data == RETRO_HW_FRAME_BUFFER_VALID);
}

void RETRO_CALLCONV audio_sample(int16_t l, int16_t r)
{
    const int16_t s[2] = {l, r};
    if (g.cb.on_audio)
        g.cb.on_audio(s, 1);
}

size_t RETRO_CALLCONV audio_batch(const int16_t* data, size_t frames)
{
    if (g.cb.on_audio)
        g.cb.on_audio(data, frames);
    return frames;
}

void RETRO_CALLCONV input_poll()
{
    if (g.cb.poll_input)
        g.cb.poll_input();
}

int16_t RETRO_CALLCONV input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port >= g.pads.size())
        return 0;
    const PadState& p = g.pads[port];
    switch (device & RETRO_DEVICE_MASK) {
    case RETRO_DEVICE_JOYPAD:
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
            return static_cast<int16_t>(p.buttons);
        return id < 16 ? ((p.buttons >> id) & 1) : 0;
    case RETRO_DEVICE_ANALOG:
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
            return id == RETRO_DEVICE_ID_ANALOG_X ? p.lx : p.ly;
        if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT)
            return id == RETRO_DEVICE_ID_ANALOG_X ? p.rx : p.ry;
        if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON) {
            if (id == RETRO_DEVICE_ID_JOYPAD_L2) return p.l2;
            if (id == RETRO_DEVICE_ID_JOYPAD_R2) return p.r2;
        }
        return 0;
    default:
        return 0;
    }
}

} // namespace

Engine& Engine::instance()
{
    static Engine e;
    return e;
}

void Engine::set_option(const std::string& key, const std::string& value)
{
    std::lock_guard lock(g.opt_mutex);
    g.opt_values[kOptionPrefix + key] = value;
    g.opt_dirty = true;
}

const std::vector<OptionInfo>& Engine::options() const { return g.opt_info; }

bool Engine::init(const Paths& paths, HostCallbacks cb)
{
    if (g.initialised)
        return true;
    g.cb         = std::move(cb);
    g.system_dir = paths.system_dir.string();
    g.save_dir   = paths.save_dir.string();

    retro_set_environment(environment);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    g.initialised = true;
    return true;
}

bool Engine::load_game(const std::filesystem::path& game)
{
    if (!g.initialised || g.loaded)
        return false;
    g.shutdown_requested = false;
    g.hw_set             = false;
    dreamstation_vmu_release_all();     // every VMU plugged in for a new game

    // The engine is initialised once and stays up for the life of the
    // process. Cycling it per game re-reserves its address space, which can
    // land at a new address while the CPU cores still point into the old one
    // (seen as a crash in Sh4Interpreter::Reset on the second game).
    if (!g.core_up) {
        retro_init();
        g.core_up = true;
    }

    if (g.netplay)
        dreamstation_netplay_configure(true, g.netplay->host, g.netplay->peer.c_str(),
                                       std::clamp(g.netplay->delay, 0, 20));
    else
        dreamstation_netplay_configure(false, false, "", 0);

    bool ok;
    if (game.empty()) {
        ok = retro_load_game(nullptr);
    } else {
        const std::string path = game.string();
        retro_game_info info{};
        info.path = path.c_str();
        ok = retro_load_game(&info);
    }
    if (ok && !g.hw_set) {
        log_line(LogLevel::Error, "engine did not negotiate an OpenGL renderer");
        retro_unload_game();
        ok = false;
    }
    if (!ok)
        return false;
    retro_get_system_av_info(&g.av);
    for (unsigned port = 0; port < g.pads.size(); ++port)
        retro_set_controller_port_device(port, g.port_devices[port]);
    g.loaded = true;
    // Connect to the other player in the background; frames run once in step.
    if (g.netplay)
        dreamstation_netplay_start();
    return true;
}

void Engine::context_reset()
{
    if (g.loaded && g.hw.context_reset) {
        g.hw.context_reset();
        g.context_live = true;
    }
}

void Engine::context_destroy()
{
    if (g.context_live && g.hw.context_destroy)
        g.hw.context_destroy();
    g.context_live = false;
}

void Engine::run_frame()
{
    if (!g.loaded || !g.context_live)
        return;
    // A netplay game starts once both sides are connected and in step.
    if (g.netplay && dreamstation_netplay_poll(nullptr, 0) != 2)
        return;
    retro_run();
}

void Engine::reset()
{
    if (g.loaded)
        retro_reset();
}

void Engine::unload_game()
{
    if (!g.loaded)
        return;
    if (g.netplay)
        dreamstation_netplay_cancel();     // ends the session, waits for its thread
    retro_unload_game();
    g.loaded       = false;
    g.context_live = false;
}

void Engine::shutdown()
{
    unload_game();
    if (g.core_up) {
        retro_deinit();
        g.core_up = false;
    }
    g.initialised = false;
    g.cb = {};
}

bool Engine::game_loaded() const        { return g.loaded; }
bool Engine::shutdown_requested() const { return g.shutdown_requested; }
double Engine::refresh_rate() const     { return g.av.timing.fps; }
double Engine::sample_rate() const      { return g.av.timing.sample_rate; }
unsigned Engine::base_width() const     { return g.av.geometry.base_width; }
unsigned Engine::base_height() const    { return g.av.geometry.base_height; }
unsigned Engine::max_width() const      { return g.av.geometry.max_width; }
unsigned Engine::max_height() const     { return g.av.geometry.max_height; }
bool Engine::bottom_left_origin() const { return g.hw.bottom_left_origin; }

float Engine::aspect_ratio() const
{
    return g.av.geometry.aspect_ratio > 0.f ? g.av.geometry.aspect_ratio : 4.f / 3.f;
}

PadState& Engine::pad(unsigned port) { return g.pads[port % g.pads.size()]; }

std::optional<Engine::ArcadeInputs> Engine::arcade_inputs() const
{
    if (!g.loaded || !dreamstation_arcade_input_name(0, RETRO_DEVICE_ID_JOYPAD_START))
        return std::nullopt;
    // The cabinet's own names for buttons it labels generically.
    static const char* const kGeneric[16] = {
        "Button 1", "Button 3", "Coin", "Start", "Up", "Down", "Left", "Right",
        "Button 2", "Button 4", "Button 6", "Button 5", "Button 8", "Button 7", "Test", "Service",
    };
    ArcadeInputs in;
    for (unsigned id = 0; id < 16; ++id) {
        const char* name = dreamstation_arcade_input_name(0, id);
        in.buttons[id] = !name ? "" : *name ? name : kGeneric[id];
    }
    for (unsigned axis = 0; axis < 4; ++axis) {
        const char* name = dreamstation_arcade_input_name(1, axis);
        in.axes[axis] = name ? (*name ? name : "Axis " + std::to_string(axis + 1)) : "";
    }
    return in;
}
void Engine::set_fast_forward(bool on) { g.fast_forward = on; }

void Engine::set_port_device(unsigned port, PortDevice device)
{
    if (port >= g.port_devices.size())
        return;
    // Sub-types of the joypad, as numbered by the engine's shell.
    unsigned id = RETRO_DEVICE_JOYPAD;
    switch (device) {
    case PortDevice::None:        id = RETRO_DEVICE_NONE; break;
    case PortDevice::Controller:  id = RETRO_DEVICE_JOYPAD; break;
    case PortDevice::ArcadeStick: id = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 3); break;
    case PortDevice::TwinStick:   id = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1); break;
    case PortDevice::RacingWheel: id = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 7); break;
    }
    g.port_devices[port] = id;
    if (g.loaded)
        retro_set_controller_port_device(port, id);
}

bool Engine::change_disc(const std::filesystem::path& image)
{
    const auto& d = g.disk;
    if (!g.loaded || !d.set_eject_state || !d.replace_image_index || !d.set_image_index ||
        !d.get_num_images || !d.add_image_index)
        return false;
    if (!d.set_eject_state(true))
        return false;
    unsigned index = 0;
    if (d.get_num_images() == 0) {
        if (!d.add_image_index())
            return false;
    } else if (d.get_image_index) {
        index = d.get_image_index();
        if (index >= d.get_num_images())
            index = 0;
    }
    const std::string path = image.string();
    retro_game_info info{};
    info.path = path.c_str();
    if (!d.replace_image_index(index, &info) || !d.set_image_index(index))
        return false;
    return d.set_eject_state(false);
}

bool Engine::open_tray()
{
    return g.loaded && g.disk.set_eject_state && g.disk.set_eject_state(true);
}

std::filesystem::path Engine::current_disc() const
{
    const auto& d = g.disk;
    if (!g.loaded || !d.get_image_path || !d.get_image_index)
        return {};
    if (d.get_eject_state && d.get_eject_state())
        return {};
    char buf[4096];
    return d.get_image_path(d.get_image_index(), buf, sizeof(buf)) ? std::filesystem::path(buf)
                                                                   : std::filesystem::path{};
}

bool Engine::serialize(std::vector<char>& out)
{
    if (!g.loaded)
        return false;
    const size_t size = retro_serialize_size();
    if (size == 0)
        return false;
    out.resize(size);
    return retro_serialize(out.data(), size);
}

bool Engine::unserialize(const std::vector<char>& in)
{
    return g.loaded && !in.empty() && retro_unserialize(in.data(), in.size());
}

bool Engine::vmu_screen(unsigned port, unsigned slot, VmuDots& dots, std::uint64_t& serial) const
{
    unsigned long long s = 0;
    if (!g.loaded || !dreamstation_vmu_screen(int(port), int(slot), dots.data(), &s))
        return false;
    serial = s;
    return true;
}

bool Engine::vmu_present(unsigned port, unsigned slot) const
{
    return g.loaded && dreamstation_vmu_present(int(port), int(slot));
}

std::filesystem::path Engine::vmu_path(unsigned port, unsigned slot) const
{
    const char* path = g.loaded ? dreamstation_vmu_path(int(port), int(slot)) : nullptr;
    return path ? std::filesystem::path(path) : std::filesystem::path{};
}

void Engine::set_netplay(const std::optional<Netplay>& netplay)
{
    g.netplay = netplay;
}

Engine::NetplayState Engine::netplay_state(std::string* message) const
{
    char text[256] = {};
    const int state = dreamstation_netplay_poll(text, sizeof text);
    if (message)
        *message = text;
    switch (state) {
    case 1:  return NetplayState::Connecting;
    case 2:  return NetplayState::Running;
    case -1: return NetplayState::Failed;
    default: return NetplayState::Off;
    }
}

void Engine::cancel_netplay()
{
    dreamstation_netplay_cancel();
}

void Engine::set_dns(const std::string& server)
{
    dreamstation_set_dns(server.c_str());
}

void Engine::set_vmu_unplugged(unsigned port, unsigned slot, bool unplugged)
{
    if (g.loaded)
        dreamstation_vmu_hold(int(port), int(slot), unplugged);
}

// State files are zstd-compressed behind a small header (a raw state is tens
// of megabytes, mostly RAM that compresses very well). Files without the
// header are read as raw states, which is what older builds wrote.
namespace {
constexpr char kStateMagic[8] = {'D', 'S', 'S', 'T', 'A', 'T', 'E', '1'};
}

bool Engine::save_state(const std::filesystem::path& file)
{
    std::vector<char> raw;
    if (!serialize(raw))
        return false;
    std::vector<char> packed(ZSTD_compressBound(raw.size()));
    const size_t n = ZSTD_compress(packed.data(), packed.size(), raw.data(), raw.size(), 3);
    if (ZSTD_isError(n))
        return false;
    const std::uint64_t raw_size = raw.size();
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(kStateMagic, sizeof(kStateMagic));
    out.write(reinterpret_cast<const char*>(&raw_size), sizeof(raw_size));
    out.write(packed.data(), static_cast<std::streamsize>(n));
    return out.good();
}

bool Engine::load_state(const std::filesystem::path& file)
{
    if (!g.loaded)
        return false;
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in)
        return false;
    const std::streamsize size = in.tellg();
    if (size <= 0)
        return false;
    in.seekg(0);
    std::vector<char> buf(static_cast<size_t>(size));
    if (!in.read(buf.data(), size))
        return false;

    const size_t header = sizeof(kStateMagic) + sizeof(std::uint64_t);
    if (buf.size() > header && std::memcmp(buf.data(), kStateMagic, sizeof(kStateMagic)) == 0) {
        std::uint64_t raw_size = 0;
        std::memcpy(&raw_size, buf.data() + sizeof(kStateMagic), sizeof(raw_size));
        if (raw_size == 0 || raw_size > (1ull << 31))
            return false;
        std::vector<char> raw(static_cast<size_t>(raw_size));
        const size_t n = ZSTD_decompress(raw.data(), raw.size(), buf.data() + header,
                                         buf.size() - header);
        if (ZSTD_isError(n) || n != raw.size())
            return false;
        return unserialize(raw);
    }
    return unserialize(buf);
}

} // namespace ds::engine
