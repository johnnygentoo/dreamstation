// SPDX-License-Identifier: GPL-3.0-or-later
#include "vmu_unit.hpp"
#include "../host/pad_input.hpp"

#include <SDL3/SDL.h>
#include <dreamstation/common/log.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fstream>

namespace ds::frontend {

namespace fs = std::filesystem;

namespace {

constexpr int kAudioRate = 48000;

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

fs::path find_ignoring_case(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (lower(e.path().filename().string()) == lower(name))
            return e.path();
    return {};
}

void rounded(const Cairo::RefPtr<Cairo::Context>& cr, double x, double y, double w, double h, double r) {
    cr->begin_new_sub_path();
    cr->arc(x + w - r, y + r, r, -M_PI / 2, 0);
    cr->arc(x + w - r, y + h - r, r, 0, M_PI / 2);
    cr->arc(x + r, y + h - r, r, M_PI / 2, M_PI);
    cr->arc(x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cr->close_path();
}

// Theme colours (JohnnyCyan) for the body; the LCD keeps the VMU's own.
void cyan(const Cairo::RefPtr<Cairo::Context>& cr, double a = 1) { cr->set_source_rgba(0x15 / 255., 0xF4 / 255., 0xEE / 255., a); }
void navy(const Cairo::RefPtr<Cairo::Context>& cr) { cr->set_source_rgb(0x16 / 255., 0x23 / 255., 0x30 / 255.); }

unsigned key_bit(guint keyval) {
    switch (keyval) {
    case GDK_KEY_Up:    case GDK_KEY_w: case GDK_KEY_W: return VmuCpu::Up;
    case GDK_KEY_Down:  case GDK_KEY_s: case GDK_KEY_S: return VmuCpu::Down;
    case GDK_KEY_Left:  case GDK_KEY_a: case GDK_KEY_A: return VmuCpu::Left;
    case GDK_KEY_Right: case GDK_KEY_d: case GDK_KEY_D: return VmuCpu::Right;
    case GDK_KEY_z: case GDK_KEY_Z: case GDK_KEY_Return: case GDK_KEY_KP_Enter: return VmuCpu::A;
    case GDK_KEY_x: case GDK_KEY_X: case GDK_KEY_BackSpace: return VmuCpu::B;
    case GDK_KEY_m: case GDK_KEY_M: return VmuCpu::Mode;
    case GDK_KEY_q: case GDK_KEY_Q: return VmuCpu::Sleep;
    default: return 0;
    }
}

} // namespace

VmuUnit::VmuUnit(int volume) : Gtk::Box(Gtk::Orientation::VERTICAL, 6) {
    m_gain = static_cast<float>(std::clamp(volume, 0, 200)) / 100.f;
    m_area.set_vexpand(true);
    m_area.set_hexpand(true);
    m_area.set_content_width(260);
    m_area.set_content_height(380);
    m_area.set_draw_func(sigc::mem_fun(*this, &VmuUnit::draw));
    append(m_area);
    m_status.set_xalign(0.5f);
    m_status.set_wrap(true);
    m_status.set_justify(Gtk::Justification::CENTER);
    append(m_status);

    // Buttons drawn on screen, held while the mouse is down on them.
    auto click = Gtk::GestureClick::create();
    click->signal_pressed().connect([this](int, double x, double y) {
        for (const auto& [bit, r] : m_button_rects)
            if (x >= r.get_x() && x < r.get_x() + r.get_width() && y >= r.get_y() &&
                y < r.get_y() + r.get_height())
                m_mouse_buttons |= bit;
        m_area.queue_draw();
    });
    click->signal_released().connect([this](int, double, double) { m_mouse_buttons = 0; m_area.queue_draw(); });
    click->signal_stopped().connect([this] { m_mouse_buttons = 0; });
    m_area.add_controller(click);

    m_cpu.set_audio_rate(kAudioRate);
    m_cpu.on_note = [this](const std::string& text) {
        ds::common::log()->warn("VMU: {}", text);
        m_status.set_text(text);
    };
}

VmuUnit::~VmuUnit() {
    if (m_playing)
        persist();
    close_audio();
}

std::string VmuUnit::game_title() const {
    const std::string& t = m_game.long_description.empty() ? m_game.description : m_game.long_description;
    return t.empty() ? m_game.name : t;
}

void VmuUnit::show_screen(const std::uint8_t* dots) {
    m_has_screen = dots != nullptr;
    if (dots)
        std::copy(dots, dots + m_screen.size(), m_screen.begin());
    if (!m_playing)
        m_area.queue_draw();
}

bool VmuUnit::key(guint keyval, bool pressed) {
    const unsigned bit = key_bit(keyval);
    if (pressed) m_key_buttons |= bit;
    else         m_key_buttons &= ~bit;
    return bit != 0 && m_playing;
}

// ── Loading and saving ────────────────────────────────────────────────────

bool VmuUnit::play(const fs::path& source, std::string& error) {
    if (m_playing)
        stop();
    if (!load(source, error))
        return false;
    m_playing = true;
    m_last_frame = 0;
    open_audio();
    start();
    m_tick_id = m_area.add_tick_callback(sigc::mem_fun(*this, &VmuUnit::on_tick));
    return true;
}

void VmuUnit::stop() {
    if (!m_playing)
        return;
    persist();
    m_playing = false;
    m_ended = false;
    if (m_tick_id) {
        m_area.remove_tick_callback(m_tick_id);
        m_tick_id = 0;
    }
    close_audio();
    m_area.queue_draw();
}

bool VmuUnit::load(const fs::path& source, std::string& error) {
    m_path = source;
    std::error_code ec;
    const auto size = fs::file_size(source, ec);
    const std::string ext = lower(source.extension().string());

    std::optional<VmuImage::Incoming> file;
    if (!ec && size == VmuImage::kImageSize) {
        m_source = Source::Card;
        auto card = VmuImage::load(source);
        if (!card || !card->valid()) {
            error = source.filename().string() + " is not a formatted VMU image.";
            return false;
        }
        m_card = *card;
    } else if (ext == ".dci") {
        m_source = Source::Dci;
        file = VmuImage::parse_dci(read_file(source), error);
    } else if (ext == ".vms" || ext == ".vmi") {
        m_source = Source::Vms;
        fs::path vms = source;
        std::vector<std::uint8_t> vmi;
        if (ext == ".vmi") {
            vmi = read_file(source);
            vms = find_ignoring_case(source.parent_path(), VmuImage::vmi_resource(vmi) + ".vms");
            if (vms.empty())   // "88.VMI" + "88.VMS", as save sites number them
                vms = find_ignoring_case(source.parent_path(), source.stem().string() + ".vms");
            if (vms.empty()) {
                error = "The VMS file named by " + source.filename().string() + " was not found.";
                return false;
            }
            m_path = vms;
        }
        const auto data = read_file(vms);
        m_vms_size = data.size();
        file = VmuImage::parse_vms(data, vmi.empty() ? nullptr : &vmi, vms.stem().string(), error);
    } else {
        error = source.filename().string() + " is not a VMU card, VMS or DCI file.";
        return false;
    }
    if (m_source != Source::Card) {
        if (!file)
            return false;
        if (!file->game()) {
            error = file->name() + " is save data, not a VMU mini-game.";
            return false;
        }
        m_dci_entry = file->entry;
        // Run it on a card of its own, as the VMU would.
        m_card = VmuImage::formatted();
        if (!m_card.add(*file, false, error))
            return false;
    }
    const auto games = m_card.files();
    const auto it = std::find_if(games.begin(), games.end(), [](const auto& f) { return f.game; });
    if (it == games.end()) {
        error = "There is no mini-game on " + source.filename().string() + ".";
        return false;
    }
    if (it->first_block != 0) {
        error = it->name + " does not start at block 0, so it cannot run.";
        return false;
    }
    m_game = *it;
    m_cpu.load(m_card.bytes(), m_game.blocks);
    const auto& flash = m_cpu.flash();
    m_saved.assign(flash.begin(), flash.begin() + m_game.blocks * static_cast<int>(VmuImage::kBlockSize));
    return true;
}

void VmuUnit::start() {
    m_cpu.boot(static_cast<std::int64_t>(std::time(nullptr)));
    m_ended = false;
    m_status.set_text("");
}

// Saves what the game wrote to its flash back to where it came from.
void VmuUnit::persist() {
    if (!m_playing)
        return;
    const auto& flash = m_cpu.flash();
    const std::size_t bytes = m_saved.size();
    if (std::equal(m_saved.begin(), m_saved.end(), flash.begin()))
        return;
    std::vector<std::uint8_t> data(flash.begin(), flash.begin() + static_cast<std::ptrdiff_t>(bytes));
    bool ok = false;
    switch (m_source) {
    case Source::Card: {
        // Merge into the card as it is on disk now: only the game's blocks change.
        auto disk = VmuImage::load(m_path);
        const auto game = disk ? disk->find(m_game.name) : std::nullopt;
        if (!disk || !game || !game->game || game->first_block != 0 || game->blocks != m_game.blocks) {
            m_status.set_text("The card changed while playing; progress was not saved.");
            return;
        }
        auto bytes_now = disk->bytes();
        std::copy(data.begin(), data.end(), bytes_now.begin());
        ok = VmuImage::from_bytes(bytes_now).save(m_path);
        break;
    }
    case Source::Vms: {
        std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(std::min(data.size(), m_vms_size)));
        ok = out.good();
        break;
    }
    case Source::Dci: {
        std::vector<std::uint8_t> dci(m_dci_entry.begin(), m_dci_entry.end());
        for (std::size_t i = 0; i + 3 < data.size(); i += 4)
            dci.insert(dci.end(), {data[i + 3], data[i + 2], data[i + 1], data[i]});
        std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(dci.data()), static_cast<std::streamsize>(dci.size()));
        ok = out.good();
        break;
    }
    }
    if (ok) {
        m_saved = data;
        ds::common::log()->info("VMU: saved {}'s progress to {}", m_game.name, m_path.string());
    } else {
        m_status.set_text("Could not save progress to " + m_path.string());
    }
}

// ── Running ───────────────────────────────────────────────────────────────

void VmuUnit::open_audio() {
    if (m_audio)
        return;
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec spec{SDL_AUDIO_S16, 1, kAudioRate};
    m_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (m_audio)
        SDL_ResumeAudioStreamDevice(m_audio);
    else
        ds::common::log()->warn("VMU: no audio output: {}", SDL_GetError());
}

void VmuUnit::close_audio() {
    if (m_audio) {
        SDL_DestroyAudioStream(m_audio);
        m_audio = nullptr;
    }
}

// The gamepad plays the VMU only while its window is the one in front, so
// it never drives a game and the VMU at once.
unsigned VmuUnit::pad_buttons() const {
    const auto* window = dynamic_cast<const Gtk::Window*>(get_root());
    if (!window || !window->is_active())
        return 0;
    auto& pad = PadInput::instance();
    const unsigned nav = pad.nav_state();
    unsigned out = 0;
    if (nav & PadInput::NavUp)     out |= VmuCpu::Up;
    if (nav & PadInput::NavDown)   out |= VmuCpu::Down;
    if (nav & PadInput::NavLeft)   out |= VmuCpu::Left;
    if (nav & PadInput::NavRight)  out |= VmuCpu::Right;
    if (nav & PadInput::NavAccept) out |= VmuCpu::A;
    if (nav & PadInput::NavBack)   out |= VmuCpu::B;
    if (nav & PadInput::NavStart)  out |= VmuCpu::Mode;
    if (pad.held_buttons() & (1u << SDL_GAMEPAD_BUTTON_BACK)) out |= VmuCpu::Sleep;
    return out;
}

bool VmuUnit::on_tick(const Glib::RefPtr<Gdk::FrameClock>& clock) {
    const std::int64_t now = clock->get_frame_time();
    const double dt = m_last_frame ? std::clamp(static_cast<double>(now - m_last_frame) / 1e6, 0.0, 0.1) : 0.0;
    m_last_frame = now;
    const unsigned buttons = m_key_buttons | m_mouse_buttons | m_test_buttons | pad_buttons();

    if (m_ended) {
        // Back at the "clock": A starts the game again.
        if (buttons & VmuCpu::A)
            start();
        m_area.queue_draw();
        return true;
    }
    m_cpu.set_buttons(buttons);
    m_cpu.run(dt);
    auto samples = m_cpu.take_audio();
    if (m_audio && !samples.empty() &&
        SDL_GetAudioStreamQueued(m_audio) < kAudioRate / 5 * 2) {   // keep latency under 0.2 s
        for (auto& s : samples)
            s = static_cast<std::int16_t>(std::clamp(s * m_gain, -32767.f, 32767.f));
        SDL_PutAudioStreamData(m_audio, samples.data(), static_cast<int>(samples.size() * 2));
    }
    if (m_cpu.exited()) {
        m_ended = true;
        persist();
        if (m_status.get_text().empty())
            m_status.set_text("The game has ended. Press A to play again.");
        signal_ended.emit();
    }
    m_area.queue_draw();
    return true;
}

// ── Drawing ───────────────────────────────────────────────────────────────

void VmuUnit::draw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
    // Body.
    const double bw = std::min<double>(width - 8, height * 0.68);
    const double bh = std::min<double>(height - 8, bw / 0.68);
    const double bx = (width - bw) / 2, by = (height - bh) / 2;
    rounded(cr, bx, by, bw, bh, bw * 0.14);
    navy(cr);
    cr->fill_preserve();
    cyan(cr, 0.8);
    cr->set_line_width(2);
    cr->stroke();

    // LCD: 48x32 at a whole-pixel scale, with the status icons below.
    const int scale = std::max(2, static_cast<int>((bw * 0.8) / VmuCpu::kWidth));
    const double lw = VmuCpu::kWidth * scale, lh = VmuCpu::kHeight * scale;
    const double lx = bx + (bw - lw) / 2, ly = by + bh * 0.08;
    const double icon_h = scale * 4.0;
    rounded(cr, lx - scale * 2, ly - scale * 2, lw + scale * 4, lh + icon_h + scale * 5, scale * 2);
    cr->set_source_rgb(0.62, 0.70, 0.63);
    cr->fill();
    const bool cpu_on = m_playing && m_cpu.display_on() && !m_ended;
    const bool mirror = !m_playing && m_has_screen;
    const double gap = scale >= 4 ? 1.0 : 0.0;
    for (int y = 0; y < VmuCpu::kHeight; ++y)
        for (int x = 0; x < VmuCpu::kWidth; ++x) {
            const bool lit = cpu_on ? m_cpu.pixel(x, y)
                           : mirror ? m_screen[static_cast<std::size_t>(y * VmuCpu::kWidth + x)] != 0
                                    : false;
            if (lit) cr->set_source_rgb(0.09, 0.13, 0.11);
            else     cr->set_source_rgb(0.58, 0.66, 0.59);
            cr->rectangle(lx + x * scale, ly + y * scale, scale - gap, scale - gap);
            cr->fill();
        }
    const auto icons = m_cpu.icons();
    const char* names[] = {"FILE", "GAME", "CLOCK", "FLASH"};
    const bool lit[] = {icons.file, icons.game, icons.clock, icons.flash};
    cr->select_font_face("sans", Cairo::ToyFontFace::Slant::NORMAL, Cairo::ToyFontFace::Weight::BOLD);
    cr->set_font_size(scale * 2.4);
    for (int i = 0; i < 4; ++i) {
        Cairo::TextExtents ext;
        cr->get_text_extents(names[i], ext);
        const double cx = lx + lw * (i + 0.5) / 4;
        cr->move_to(cx - ext.width / 2, ly + lh + icon_h);
        if (cpu_on && lit[i]) cr->set_source_rgb(0.09, 0.13, 0.11);
        else                  cr->set_source_rgb(0.56, 0.64, 0.57);
        cr->show_text(names[i]);
    }
    if (m_playing && m_ended) {
        cr->set_font_size(scale * 3);
        cr->set_source_rgb(0.09, 0.13, 0.11);
        Cairo::TextExtents ext;
        cr->get_text_extents("PRESS A", ext);
        cr->move_to(lx + (lw - ext.width) / 2, ly + lh / 2 + ext.height / 2);
        cr->show_text("PRESS A");
    }

    // Buttons, remembered for clicking.
    m_button_rects.clear();
    const unsigned held = m_playing ? (m_key_buttons | m_mouse_buttons | m_test_buttons) : 0;
    const double unit = bw * 0.085;
    const auto button = [&](unsigned bit, double cx, double cy, double w, double h, double r, const char* label) {
        rounded(cr, cx - w / 2, cy - h / 2, w, h, r);
        if (held & bit) cyan(cr); else cr->set_source_rgb(0x1E / 255., 0x2E / 255., 0x3E / 255.);
        cr->fill_preserve();
        cyan(cr, m_playing ? 0.9 : 0.45);
        cr->set_line_width(1.5);
        cr->stroke();
        if (label) {
            cr->set_font_size(unit * 0.55);
            Cairo::TextExtents ext;
            cr->get_text_extents(label, ext);
            cr->move_to(cx - ext.width / 2 - ext.x_bearing, cy - ext.height / 2 - ext.y_bearing);
            if (held & bit) navy(cr); else cyan(cr, m_playing ? 1.0 : 0.5);
            cr->show_text(label);
        }
        m_button_rects[bit] = Gdk::Rectangle(static_cast<int>(cx - w / 2), static_cast<int>(cy - h / 2),
                                             static_cast<int>(w), static_cast<int>(h));
    };
    const double pad_y = ly + lh + icon_h + bh * 0.22;
    const double dx = bx + bw * 0.28;
    button(VmuCpu::Up,    dx, pad_y - unit, unit, unit, unit * 0.2, nullptr);
    button(VmuCpu::Down,  dx, pad_y + unit, unit, unit, unit * 0.2, nullptr);
    button(VmuCpu::Left,  dx - unit, pad_y, unit, unit, unit * 0.2, nullptr);
    button(VmuCpu::Right, dx + unit, pad_y, unit, unit, unit * 0.2, nullptr);
    button(VmuCpu::B, bx + bw * 0.64, pad_y + unit * 0.55, unit * 1.3, unit * 1.3, unit * 0.65, "B");
    button(VmuCpu::A, bx + bw * 0.80, pad_y - unit * 0.55, unit * 1.3, unit * 1.3, unit * 0.65, "A");
    const double small_y = pad_y + unit * 2.6;
    button(VmuCpu::Sleep, bx + bw * 0.40, small_y, unit * 1.9, unit * 0.65, unit * 0.32, "SLEEP");
    button(VmuCpu::Mode,  bx + bw * 0.62, small_y, unit * 1.9, unit * 0.65, unit * 0.32, "MODE");
}

} // namespace ds::frontend
