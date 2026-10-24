// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gtkmm.h>
#include <dreamstation/engine/engine.hpp>
#include <epoxy/gl.h>
#include <dreamstation/common/types.hpp>
#include "post_chain.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <vector>

struct SDL_AudioStream;

namespace ds::frontend {


// RenderWidget is the game view: a GtkGLArea that drives the compiled-in
// engine. Each display tick it runs the emulated frames that are due, has the
// engine render into an off-screen framebuffer at the internal resolution,
// then presents that image through the selected post-processing shader,
// letterboxed to the game's aspect ratio. Audio and pads are serviced here
// too, since the engine samples both from inside run_frame().
class RenderWidget : public Gtk::GLArea {
public:
    struct Session {
        std::filesystem::path game;         // empty = boot the BIOS, no disc
        std::filesystem::path system_dir;   // contains dc/dc_boot.bin
        std::filesystem::path save_dir;
        std::map<std::string, std::string> options;  // engine options
        std::optional<ds::engine::Engine::Netplay> netplay;   // online with a friend
        std::string dns;                                      // Dreamcast DNS, "" = default
    };

    RenderWidget();
    ~RenderWidget() override;

    // Thread-affinity for everything below: GTK main thread.
    // The post-processing chain applied when presenting the engine image.
    void set_post_chain(std::vector<PostStage> stages, bool enabled, bool native_input = false);
    void set_post_option(std::size_t stage, const std::string& name, float value);

    // Queue a session; the game is loaded on the next render (the engine
    // needs the GL context current). Result arrives via signal_started.
    void start(Session session);
    void stop();
    void reset();
    void set_paused(bool paused);
    void set_fast_forward(bool on);
    void set_option(const std::string& key, const std::string& value);
    bool running() const { return m_running; }

    bool save_state(const std::filesystem::path& file);
    bool load_state(const std::filesystem::path& file);
    bool save_state_to(std::vector<char>& buffer);
    bool load_state_from(const std::vector<char>& buffer);
    // Load this state once the freshly booted game has run a few frames
    // (the engine cannot restore a state before its first frame).
    void load_state_after_boot(const std::filesystem::path& file);

    // The current engine image at internal resolution (null if none yet).
    Glib::RefPtr<Gdk::Pixbuf> capture_frame();

    bool paused() const { return m_paused; }
    bool fast_forward() const { return m_fast_forward; }
    void set_muted(bool muted) { m_muted = muted; }
    bool muted() const { return m_muted; }
    // Output volume (percent, 0..200), playback device ("" = default) and
    // how much audio to keep queued.
    void set_audio(int volume_percent, bool muted, const std::string& device, int buffer_ms);
    // Fit of the image in the window: aspect "auto" | "4:3" | "16:9" |
    // "stretch", nearest or bilinear sampling, optional integer scaling.
    void set_display(const std::string& aspect, bool nearest, bool integer);
    // Emulation speed in percent of real time (100 = normal).
    void set_speed(int percent);
    int  speed() const { return m_speed; }
    // While paused: emulate exactly one frame.
    void step_frame();
    bool change_disc(const std::filesystem::path& image);
    bool open_tray();
    void set_cursor_hidden(bool hidden);
    // Options known to the engine (available once a session has started,
    // or after prepare_engine()).
    void prepare_engine(const std::filesystem::path& system_dir,
                        const std::filesystem::path& save_dir);

    // Counters for the FPS display.
    u64 frames_presented() const { return m_frames_presented; }
    u64 frames_emulated()  const { return m_frames_emulated; }
    // Size of the drawing area in device pixels, and a signal when it changes.
    int view_width()  const { return m_fb_width; }
    int view_height() const { return m_fb_height; }
    sigc::signal<void()> signal_view_resized;
    // Average and worst emulated-frame time since the last call, in ms.
    void take_frame_times(double& average_ms, double& worst_ms);
    unsigned frame_width()  const { return m_frame_w; }
    unsigned frame_height() const { return m_frame_h; }

    // Keyboard fallback for port A; returns true when the key is bound.
    bool handle_key(guint keyval, bool pressed);

    sigc::signal<void(bool ok, std::string error)> signal_started;
    sigc::signal<void(std::string text)>           signal_message;
    sigc::signal<void()>                           signal_engine_shutdown;

protected:
    void on_realize()                                         override;
    void on_unrealize()                                       override;
    bool on_render(const Glib::RefPtr<Gdk::GLContext>& ctx)  override;
    void on_resize(int w, int h)                             override;

private:
    void ensure_target(unsigned w, unsigned h);
    void destroy_target();
    bool begin_session();
    void end_session();
    void present();
    void open_audio(int sample_rate);
    void close_audio();
    void selftest_capture();
    void ensure_engine();

    PostChain m_post;

    // Engine render target (internal resolution).
    GLuint   m_fbo = 0, m_color = 0, m_depth = 0;
    unsigned m_target_w = 0, m_target_h = 0;
    unsigned m_frame_w = 0, m_frame_h = 0;

    Session  m_session;
    bool     m_pending_start = false;
    bool     m_engine_ready  = false;   // Engine::init() done (once per process)
    bool     m_running       = false;
    bool     m_paused        = false;
    bool     m_gl_ready      = false;
    bool     m_have_frame    = false;
    bool     m_fast_forward  = false;
    bool     m_muted         = false;
    std::filesystem::path m_boot_state;      // state to load after boot

    // Pacing: emulated frames are released at the engine's refresh rate.
    using clock = std::chrono::steady_clock;
    clock::time_point m_next_frame{};
    clock::time_point m_session_start{};
    u64    m_render_calls = 0;
    double m_display_hz = 0.0;
    double m_run_seconds = 0.0;
    double m_stat_seconds = 0.0, m_stat_worst = 0.0;
    u64    m_stat_frames  = 0;
    clock::time_point  m_last_render{};
    std::vector<float> m_intervals;   // self-test statistics
    std::vector<float> m_render_ms;
    u64 m_frames_presented = 0;
    u64 m_frames_emulated  = 0;

    SDL_AudioStream* m_audio = nullptr;
    int              m_audio_rate = 0;
    float            m_audio_ratio = 1.f;
    int              m_volume = 100;
    int              m_buffer_ms = 60;
    std::string      m_audio_device;
    std::string      m_aspect_mode = "auto";
    bool             m_nearest = false;
    bool             m_integer = false;
    int              m_speed   = 100;
    int              m_step_frames = 0;
    guint m_tick_id = 0;

    int m_fb_width  = 0;
    int m_fb_height = 0;
};

} // namespace ds::frontend
