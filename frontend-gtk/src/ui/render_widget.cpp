// SPDX-License-Identifier: GPL-3.0-or-later
#include "render_widget.hpp"
#include "../image_io.hpp"
#include "../host/pad_input.hpp"
#include <dreamstation/common/log.hpp>
#include <dreamstation/engine/engine.hpp>

#include <SDL3/SDL.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <gdk/gdkkeysyms.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <dlfcn.h>
#include <vector>

namespace ds::frontend {

using ds::engine::Engine;

// GL symbol lookup for the engine. GTK4 uses EGL on Wayland and on X11 by
// default; the dlsym fallback covers a GLX context.
static void* gl_proc_address(const char* sym) {
    if (eglGetCurrentContext() != EGL_NO_CONTEXT)
        if (void* p = reinterpret_cast<void*>(eglGetProcAddress(sym)))
            return p;
    return dlsym(RTLD_DEFAULT, sym);
}

// ── RenderWidget ──────────────────────────────────────────────────────────

RenderWidget::RenderWidget() {
    set_hexpand(true);
    set_vexpand(true);
    set_auto_render(false);
    set_has_depth_buffer(false);
    set_has_stencil_buffer(false);
    set_allowed_apis(Gdk::GLApi::GL);
    set_required_version(4, 3);
    set_focusable(true);

    // Redraw every display refresh while a game runs; on_render() decides
    // whether an emulated frame is due.
    m_tick_id = add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>&) {
        if (m_running || m_pending_start)
            queue_render();
        return true;
    });
}

RenderWidget::~RenderWidget() {
    if (m_tick_id)
        remove_tick_callback(m_tick_id);
}

void RenderWidget::start(Session session) {
    if (m_running)
        stop();
    m_session       = std::move(session);
    m_pending_start = true;
    queue_render();
}

void RenderWidget::stop() {
    m_pending_start = false;
    if (!m_running)
        return;
    if (get_realized())
        make_current();
    end_session();
    queue_render();
}

void RenderWidget::reset() {
    if (m_running)
        Engine::instance().reset();
}

void RenderWidget::set_paused(bool paused) {
    m_paused = paused;
    if (m_audio) {
        if (paused) SDL_PauseAudioStreamDevice(m_audio);
        else        SDL_ResumeAudioStreamDevice(m_audio);
    }
    m_next_frame = clock::now();
}

void RenderWidget::set_fast_forward(bool on) {
    m_fast_forward = on;
    Engine::instance().set_fast_forward(on);
    if (m_audio)
        SDL_ClearAudioStream(m_audio);
    m_next_frame = clock::now();
}

void RenderWidget::take_frame_times(double& average_ms, double& worst_ms) {
    average_ms = m_stat_frames ? 1000.0 * m_stat_seconds / static_cast<double>(m_stat_frames) : 0.0;
    worst_ms   = 1000.0 * m_stat_worst;
    m_stat_seconds = m_stat_worst = 0.0;
    m_stat_frames  = 0;
}

void RenderWidget::set_cursor_hidden(bool hidden) {
    if (hidden)
        set_cursor(Gdk::Cursor::create("none"));
    else
        set_cursor(Glib::RefPtr<Gdk::Cursor>{});
}

bool RenderWidget::save_state_to(std::vector<char>& buffer) {
    return m_running && Engine::instance().serialize(buffer);
}

bool RenderWidget::load_state_from(const std::vector<char>& buffer) {
    return m_running && Engine::instance().unserialize(buffer);
}

void RenderWidget::load_state_after_boot(const std::filesystem::path& file) {
    m_boot_state = file;
}

Glib::RefPtr<Gdk::Pixbuf> RenderWidget::capture_frame() {
    if (!m_running || !m_have_frame || !m_fbo || !get_realized())
        return {};
    make_current();
    const int w = static_cast<int>(m_target_w), h = static_cast<int>(m_target_h);
    auto pixbuf = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, w, h);
    std::vector<guint8> row(static_cast<std::size_t>(w) * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    const bool flip = Engine::instance().bottom_left_origin();
    guint8* dst = pixbuf->get_pixels();
    const int stride = pixbuf->get_rowstride();
    for (int y = 0; y < h; ++y) {
        glReadPixels(0, flip ? h - 1 - y : y, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, row.data());
        guint8* out = dst + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            out[x * 3 + 0] = row[static_cast<std::size_t>(x) * 4 + 0];
            out[x * 3 + 1] = row[static_cast<std::size_t>(x) * 4 + 1];
            out[x * 3 + 2] = row[static_cast<std::size_t>(x) * 4 + 2];
        }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return pixbuf;
}

void RenderWidget::prepare_engine(const std::filesystem::path& system_dir,
                                  const std::filesystem::path& save_dir) {
    if (m_engine_ready)
        return;
    m_session.system_dir = system_dir;
    m_session.save_dir   = save_dir;
    ensure_engine();
}

void RenderWidget::set_option(const std::string& key, const std::string& value) {
    Engine::instance().set_option(key, value);
}

bool RenderWidget::save_state(const std::filesystem::path& file) {
    return m_running && Engine::instance().save_state(file);
}

bool RenderWidget::load_state(const std::filesystem::path& file) {
    return m_running && Engine::instance().load_state(file);
}

bool RenderWidget::handle_key(guint keyval, bool pressed) {
    return PadInput::instance().set_key(keyval, pressed);
}

// ── GL lifecycle ──────────────────────────────────────────────────────────

void RenderWidget::set_post_chain(std::vector<PostStage> stages, bool enabled, bool native_input) {
    m_post.set_stages(std::move(stages));
    m_post.set_enabled(enabled);
    m_post.set_native_input(native_input);
    queue_render();
}

void RenderWidget::set_post_option(std::size_t stage, const std::string& name, float value) {
    m_post.set_option(stage, name, value);
    queue_render();
}

void RenderWidget::on_realize() {
    Gtk::GLArea::on_realize();
    make_current();
    if (has_error()) {
        ds::common::log()->error("RenderWidget: no usable OpenGL context");
        return;
    }
    ds::common::log()->info("RenderWidget: {} / {}",
        reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
        reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    m_gl_ready = true;
}

void RenderWidget::on_unrealize() {
    make_current();
    if (m_running)
        end_session();
    destroy_target();
    m_post.release();
    m_gl_ready = false;
    Gtk::GLArea::on_unrealize();
}

void RenderWidget::on_resize(int w, int h) {
    const bool changed = w != m_fb_width || h != m_fb_height;
    m_fb_width  = w;
    m_fb_height = h;
    if (changed)
        signal_view_resized.emit();
}

void RenderWidget::destroy_target() {
    if (m_fbo)   glDeleteFramebuffers(1, &m_fbo);
    if (m_color) glDeleteTextures(1, &m_color);
    if (m_depth) glDeleteRenderbuffers(1, &m_depth);
    m_fbo = m_color = m_depth = 0;
    m_target_w = m_target_h = 0;
}

void RenderWidget::ensure_target(unsigned w, unsigned h) {
    if (w == 0 || h == 0 || (w == m_target_w && h == m_target_h && m_fbo))
        return;
    destroy_target();
    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glGenTextures(1, &m_color);
    glBindTexture(GL_TEXTURE_2D, m_color);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_color, 0);
    glGenRenderbuffers(1, &m_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                          static_cast<GLsizei>(w), static_cast<GLsizei>(h));
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        ds::common::log()->error("RenderWidget: engine framebuffer {}x{} incomplete", w, h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    m_target_w = w;
    m_target_h = h;
    m_have_frame = false;
}

// ── Audio ─────────────────────────────────────────────────────────────────

void RenderWidget::set_audio(int volume_percent, bool muted, const std::string& device,
                             int buffer_ms) {
    m_volume    = std::clamp(volume_percent, 0, 200);
    m_muted     = muted;
    m_buffer_ms = std::clamp(buffer_ms, 20, 300);
    if (device != m_audio_device) {
        m_audio_device = device;
        if (m_audio)
            open_audio(m_audio_rate);     // move the stream to the new device
    }
    if (m_audio)
        SDL_SetAudioStreamGain(m_audio, static_cast<float>(m_volume) / 100.f);
}

void RenderWidget::set_display(const std::string& aspect, bool nearest, bool integer) {
    m_aspect_mode = aspect;
    m_nearest     = nearest;
    m_integer     = integer;
    queue_render();
}

void RenderWidget::set_speed(int percent) {
    m_speed      = std::clamp(percent, 10, 1000);
    m_next_frame = clock::now();
    if (m_audio)
        SDL_ClearAudioStream(m_audio);
}

void RenderWidget::step_frame() {
    if (m_running && m_paused) {
        ++m_step_frames;
        queue_render();
    }
}

bool RenderWidget::change_disc(const std::filesystem::path& image) {
    return m_running && Engine::instance().change_disc(image);
}

bool RenderWidget::open_tray() {
    return m_running && Engine::instance().open_tray();
}

void RenderWidget::open_audio(int sample_rate) {
    close_audio();
    SDL_AudioSpec spec{};
    spec.format   = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq     = sample_rate;

    // Find the chosen device by name; fall back to the system default.
    SDL_AudioDeviceID device = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (!m_audio_device.empty()) {
        int count = 0;
        if (SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count)) {
            for (int i = 0; i < count; ++i) {
                const char* name = SDL_GetAudioDeviceName(ids[i]);
                if (name && m_audio_device == name) { device = ids[i]; break; }
            }
            SDL_free(ids);
        }
    }
    m_audio = SDL_OpenAudioDeviceStream(device, &spec, nullptr, nullptr);
    if (!m_audio && device != SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK)
        m_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!m_audio) {
        ds::common::log()->warn("RenderWidget: no audio device: {}", SDL_GetError());
        return;
    }
    m_audio_rate  = sample_rate;
    m_audio_ratio = 1.f;
    SDL_SetAudioStreamGain(m_audio, static_cast<float>(m_volume) / 100.f);
    SDL_ResumeAudioStreamDevice(m_audio);
}

void RenderWidget::close_audio() {
    if (m_audio) {
        SDL_DestroyAudioStream(m_audio);
        m_audio = nullptr;
    }
}

// ── Engine session ────────────────────────────────────────────────────────

void RenderWidget::ensure_engine() {
    if (m_engine_ready)
        return;
    Engine& engine = Engine::instance();
    ds::engine::HostCallbacks cb;
    cb.current_framebuffer = [this] { return static_cast<std::uintptr_t>(m_fbo); };
    cb.get_proc_address    = gl_proc_address;
    cb.on_frame = [this](unsigned w, unsigned h, bool fresh) {
        if (!fresh)
            return;
        m_frame_w    = w;
        m_frame_h    = h;
        m_have_frame = true;
        ++m_frames_presented;
    };
    cb.on_audio = [this](const std::int16_t* samples, std::size_t frames) {
        if (!m_audio || m_muted || m_fast_forward)
            return;
        // Dynamic rate control: video is locked to the display, so the
        // game's audio clock drifts slightly against the sound card.
        // Nudge playback speed (at most ±0.5%, inaudible) to hold about
        // 60 ms queued. A quarter-second backlog is dropped outright.
        const int queued = SDL_GetAudioStreamQueued(m_audio);
        const int target = m_audio_rate * 4 * m_buffer_ms / 1000;
        if (queued > std::max(m_audio_rate, target * 3)) {
            SDL_ClearAudioStream(m_audio);
        } else {
            const float err = std::clamp(
                static_cast<float>(queued - target) / static_cast<float>(target), -1.f, 1.f);
            m_audio_ratio += (1.f + 0.005f * err - m_audio_ratio) * 0.05f;
            // At other than normal speed the sound plays faster or slower with it.
            SDL_SetAudioStreamFrequencyRatio(m_audio,
                                             m_audio_ratio * static_cast<float>(m_speed) / 100.f);
        }
        SDL_PutAudioStreamData(m_audio, samples, static_cast<int>(frames * 4));
    };
    cb.poll_input = [] { PadInput::instance().update(); };
    cb.on_log = [](ds::engine::LogLevel lvl, const char* line) {
        switch (lvl) {
            case ds::engine::LogLevel::Error: ds::common::log()->error("engine: {}", line); break;
            case ds::engine::LogLevel::Warn:  ds::common::log()->warn("engine: {}", line);  break;
            case ds::engine::LogLevel::Info:  ds::common::log()->info("engine: {}", line);  break;
            default:                          ds::common::log()->debug("engine: {}", line); break;
        }
    };
    cb.on_message = [this](const char* text) { signal_message.emit(text); };
    cb.on_rumble = [](unsigned port, std::uint16_t strong, std::uint16_t weak) {
        PadInput::instance().rumble(port, strong, weak);
    };
    engine.init({m_session.system_dir, m_session.save_dir}, std::move(cb));
    m_engine_ready = true;
}

bool RenderWidget::begin_session() {
    m_pending_start = false;
    Engine& engine = Engine::instance();
    ensure_engine();
    for (const auto& [key, value] : m_session.options)
        engine.set_option(key, value);
    engine.set_netplay(m_session.netplay);
    engine.set_dns(m_session.dns);

    if (!engine.load_game(m_session.game)) {
        signal_started.emit(false, "The engine could not load this game — see the log for details");
        return false;
    }

    ensure_target(engine.base_width(), engine.base_height());
    engine.context_reset();
    open_audio(static_cast<int>(std::lround(engine.sample_rate())));

    m_running    = true;
    m_paused     = false;
    m_have_frame = false;
    m_next_frame = m_session_start = clock::now();
    m_frames_emulated = m_frames_presented = 0;
    signal_started.emit(true, {});
    return true;
}

void RenderWidget::end_session() {
    Engine& engine = Engine::instance();
    engine.context_destroy();
    engine.unload_game();
    close_audio();
    PadInput::instance().clear_keys();
    for (unsigned port = 0; port < 4; ++port)
        PadInput::instance().rumble(port, 0, 0);
    m_running      = false;
    m_paused       = false;
    m_have_frame   = false;
    m_fast_forward = false;
    m_boot_state.clear();
    engine.set_fast_forward(false);
}

// ── Frame ─────────────────────────────────────────────────────────────────

bool RenderWidget::on_render(const Glib::RefPtr<Gdk::GLContext>&) {
    if (!m_gl_ready)
        return true;
    ++m_render_calls;
    // Developer aid: redraw-interval statistics for the self-test report.
    static const bool s_stats = std::getenv("DS_SELFTEST_SHOT") != nullptr;
    if (s_stats && m_running && m_frames_emulated > 300) {
        const auto t = clock::now();
        if (m_last_render.time_since_epoch().count() != 0)
            m_intervals.push_back(std::chrono::duration<float, std::milli>(t - m_last_render).count());
        m_last_render = t;
    }
    const auto render_begin = clock::now();

    if (m_pending_start)
        begin_session();

    if (m_running && !m_paused) {
        Engine& engine = Engine::instance();
        const double emu_hz = std::max(1.0, engine.refresh_rate());
        int due = 0;

        // When the display refreshes at (very nearly) the game's rate, run
        // exactly one emulated frame per display frame: perfectly even
        // pacing, with audio absorbing the sub-1% difference. Otherwise
        // release frames against a clock.
        gint64 interval_us = 0, presentation = 0;
        GdkFrameClock* fc = gtk_widget_get_frame_clock(GTK_WIDGET(gobj()));
        if (fc)
            gdk_frame_clock_get_refresh_info(fc, 0, &interval_us, &presentation);
        const double display_hz = interval_us > 0 ? 1e6 / static_cast<double>(interval_us) : 0.0;
        m_display_hz = display_hz;
        const auto to_ticks = [](double seconds) {
            return std::chrono::duration_cast<clock::duration>(
                std::chrono::duration<double>(seconds));
        };
        const auto   now    = clock::now();
        const double period = 1.0 / (emu_hz * static_cast<double>(m_speed) / 100.0);
        const double lag    = std::chrono::duration<double>(now - m_next_frame).count();
        // Display lock only makes sense at normal speed.
        const bool   locked = m_speed == 100 && display_hz > 0.0 &&
                              std::abs(display_hz / emu_hz - 1.0) < 0.01;

        // (A fixed span of real time, so higher speeds do not resync sooner.)
        if (lag > 0.134 || lag < -0.134) {
            // Long stall (window hidden, debugger, ...): resynchronise.
            m_next_frame = now;
            due = 1;
        } else if (locked) {
            // Lock to the display: one emulated frame per redraw, with the
            // schedule advancing in display intervals so the sub-1% rate
            // difference never accumulates. If the compositor skipped
            // redraws, emulate the missed frames now so game speed and audio
            // stay correct; an extra redraw (expose) emulates nothing.
            if (lag > -period * 0.5)
                due = std::min(3, 1 + static_cast<int>(std::floor(lag / period + 0.5)));
            m_next_frame += to_ticks(due / display_hz);
        } else {
            // Above normal speed several frames may be due per redraw.
            const int cap = std::max(3, m_speed / 100 + 2);
            while (now >= m_next_frame && due < cap) {
                m_next_frame += to_ticks(period);
                ++due;
            }
        }
        // Fast forward: ignore the schedule and run as many frames as fit in
        // most of a display interval.
        const auto ff_deadline = clock::now() + std::chrono::milliseconds(11);
        if (m_fast_forward) {
            due = 64;
            m_next_frame = clock::now();
        }

        for (int i = 0; i < due; ++i) {
            if (m_fast_forward && i > 0 && clock::now() >= ff_deadline)
                break;
            // The engine may change its output size (resolution option).
            if (m_frame_w && m_frame_h && (m_frame_w != m_target_w || m_frame_h != m_target_h))
                ensure_target(m_frame_w, m_frame_h);
            glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
            const auto t0 = clock::now();
            engine.run_frame();
            const double took = std::chrono::duration<double>(clock::now() - t0).count();
            m_run_seconds += took;
            m_stat_seconds += took;
            m_stat_worst = std::max(m_stat_worst, took);
            ++m_stat_frames;
            if (s_stats && took > 0.030)
                std::fprintf(stderr, "hitch: frame %llu run_frame %.0f ms\n",
                             static_cast<unsigned long long>(m_frames_emulated), took * 1000.0);
            ++m_frames_emulated;
            if (!m_boot_state.empty() && m_frames_emulated >= 3) {
                const auto file = std::move(m_boot_state);
                m_boot_state.clear();
                if (!engine.load_state(file))
                    signal_message.emit("Could not load the saved state");
            }
        }
        if (engine.shutdown_requested()) {
            end_session();
            signal_engine_shutdown.emit();
        }
    }

    // Frame advance while paused.
    if (m_running && m_paused && m_step_frames > 0) {
        --m_step_frames;
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        Engine::instance().run_frame();
        ++m_frames_emulated;
    }

    present();
    if (s_stats && m_running)
        m_render_ms.push_back(std::chrono::duration<float, std::milli>(clock::now() - render_begin).count());
    return true;
}

void RenderWidget::present() {
    // The engine leaves arbitrary GL state behind; set everything we rely on.
    attach_buffers();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, m_fb_width, m_fb_height);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!m_running || !m_have_frame || !m_color || m_fb_width <= 0 || m_fb_height <= 0)
        return;

    // Fit the image in the window. "auto" uses the aspect the engine
    // reports (4:3, or 16:9 with widescreen on); "stretch" fills the window.
    float aspect = Engine::instance().aspect_ratio();
    if (m_aspect_mode == "4:3")       aspect = 4.f / 3.f;
    else if (m_aspect_mode == "16:9") aspect = 16.f / 9.f;
    int vw = m_fb_width, vh = m_fb_height;
    if (m_aspect_mode != "stretch") {
        vh = static_cast<int>(std::lround(static_cast<float>(vw) / aspect));
        if (vh > m_fb_height) {
            vh = m_fb_height;
            vw = static_cast<int>(std::lround(static_cast<float>(vh) * aspect));
        }
        // Integer scaling: the largest whole multiple of 480 lines that fits.
        if (m_integer && vh >= 480) {
            vh = (vh / 480) * 480;
            vw = static_cast<int>(std::lround(static_cast<float>(vh) * aspect));
        }
    }
    glBindTexture(GL_TEXTURE_2D, m_color);
    const GLint filter = m_nearest ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    PostChain::Frame frame;
    frame.source   = m_color;
    frame.source_w = static_cast<int>(m_target_w);
    frame.source_h = static_cast<int>(m_target_h);
    frame.x = (m_fb_width - vw) / 2;
    frame.y = (m_fb_height - vh) / 2;
    frame.w = vw;
    frame.h = vh;
    frame.time = static_cast<float>(g_get_monotonic_time() % 3600000000ll) / 1e6f;
    m_post.render(frame, [this] { attach_buffers(); });
    if (m_post.take_error_flag())
        signal_message.emit("Post-processing shader error — see the log");

    selftest_capture();
}

// Developer aid: with DS_SELFTEST_SHOT=<file.png> the composited window
// output is saved once DS_SELFTEST_FRAMES frames have been emulated, then
// the application quits. Lets the full app path be checked without a human.
void RenderWidget::selftest_capture() {
    static const char* path = std::getenv("DS_SELFTEST_SHOT");
    if (!path)
        return;
    static const u64 after = [] {
        const char* n = std::getenv("DS_SELFTEST_FRAMES");
        return n ? std::strtoull(n, nullptr, 10) : 600ull;
    }();
    static clock::time_point warm{};
    static u64 warm_frames = 0;
    if (m_frames_emulated >= 300 && warm_frames == 0) {
        warm = clock::now();
        warm_frames = m_frames_emulated;
    }
    if (m_frames_emulated < after)
        return;

    // Rate is measured after a 300-frame warm-up (boot, shader compilation).
    const double secs = std::chrono::duration<double>(clock::now() - warm).count();
    const double rate = static_cast<double>(m_frames_emulated - warm_frames) / secs;
    const int w = m_fb_width, h = m_fb_height;
    std::vector<guint8> px(static_cast<std::size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    std::vector<guint8> flipped(px.size());
    for (int y = 0; y < h; ++y)
        std::copy_n(&px[static_cast<std::size_t>(h - 1 - y) * w * 4], w * 4,
                    &flipped[static_cast<std::size_t>(y) * w * 4]);
    auto pixbuf = Gdk::Pixbuf::create_from_data(flipped.data(), Gdk::Colorspace::RGB, true, 8,
                                                w, h, w * 4);
    image::save_png(pixbuf, path);
    std::fprintf(stderr,
                 "selftest: %dx%d window image -> %s | engine frame %ux%u | "
                 "emulated %llu presented %llu | run_frame avg %.2f ms | renders %llu | display %.2f Hz | steady %.2f/s over %.1fs | audio ratio %.4f | pads %d\n",
                 w, h, path, m_frame_w, m_frame_h,
                 static_cast<unsigned long long>(m_frames_emulated),
                 static_cast<unsigned long long>(m_frames_presented),
                 1000.0 * m_run_seconds / static_cast<double>(std::max<u64>(1, m_frames_emulated)),
                 static_cast<unsigned long long>(m_render_calls), m_display_hz, rate, secs,
                 static_cast<double>(m_audio_ratio),
                 PadInput::instance().connected());
    if (!m_intervals.empty()) {
        auto sorted = m_intervals;
        std::sort(sorted.begin(), sorted.end());
        const auto pct = [&](double p) { return sorted[static_cast<std::size_t>(p * (sorted.size() - 1))]; };
        int over20 = 0, over30 = 0;
        for (float v : sorted) { over20 += v > 20.f; over30 += v > 30.f; }
        auto work = m_render_ms;
        std::sort(work.begin(), work.end());
        std::fprintf(stderr,
                     "pacing: %zu redraws | interval p50 %.2f p90 %.2f p99 %.2f max %.1f ms | "
                     ">20ms %d  >30ms %d | on_render p50 %.2f p99 %.2f max %.1f ms\n",
                     sorted.size(), pct(0.5), pct(0.9), pct(0.99), sorted.back(), over20, over30,
                     work[work.size() / 2], work[static_cast<std::size_t>(0.99 * (work.size() - 1))],
                     work.back());
    }
    path = nullptr;
    if (auto app = Gio::Application::get_default())
        app->quit();
}

} // namespace ds::frontend
