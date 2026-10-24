// SPDX-License-Identifier: GPL-2.0-or-later
//
// engine_probe — headless proof that the compiled-in engine boots and draws.
// Creates an off-screen EGL/OpenGL context, runs N frames of a game (or the
// BIOS when no game is given) and writes the last presented frame to a PNG,
// so the result can be looked at instead of inferred from counters.
//
//   engine_probe --system DIR [--save DIR] [--game PATH] [--frames N]
//                [--out FILE.png] [--opt key=value]... [--press BTN@FRAME]...
//                [--net-host | --net-join ADDRESS] [--net-delay N]
//                [--hold BTN@FROM-TO]... [--stick X,Y@FROM-TO]...

#include <dreamstation/engine/engine.hpp>

#include <epoxy/egl.h>
#include <epoxy/gl.h>

// Implementation is already linked in by the engine.
#include <stb_image_write.h>

#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <optional>
#include <thread>
#include <vector>

using ds::engine::Engine;

namespace {

struct Press { unsigned button; long frame; };

void* egl_proc(const char* sym)
{
    return reinterpret_cast<void*>(eglGetProcAddress(sym));
}

bool make_context(EGLDisplay& dpy, EGLContext& ctx)
{
    dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) {
        std::fprintf(stderr, "probe: EGL surfaceless display unavailable\n");
        return false;
    }
    eglBindAPI(EGL_OPENGL_API);
    const EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                               EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n == 0)
        return false;
    const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                               EGL_CONTEXT_OPENGL_PROFILE_MASK,
                               EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT)
        return false;
    return eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
}

void crash_handler(int sig)
{
    void* frames[64];
    const int n = backtrace(frames, 64);
    const char msg[] = "probe: fatal signal, backtrace:\n";
    (void)!write(2, msg, sizeof(msg) - 1);
    backtrace_symbols_fd(frames, n, 2);
    _exit(128 + sig);
}

} // namespace

int main(int argc, char** argv)
{
    std::optional<ds::engine::Engine::Netplay> netplay;
    int net_delay = 1;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (std::getenv("PROBE_BACKTRACE")) {
        static char alt[1 << 16];
        stack_t ss{};
        ss.ss_sp = alt;
        ss.ss_size = sizeof(alt);
        sigaltstack(&ss, nullptr);
        struct sigaction sa{};
        sa.sa_handler = crash_handler;
        sa.sa_flags = SA_ONSTACK;
        for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE})
            sigaction(sig, &sa, nullptr);
    }
    std::string system_dir, save_dir, game, out = "probe.png";
    long frames = 600;
    bool realtime = false;
    std::vector<std::pair<std::string, std::string>> opts;
    std::vector<Press> presses;
    struct Hold {
        int button = 0;
        bool stick = false;
        std::int16_t x = 0, y = 0;
        long from = 0, to = 0;
    };
    std::vector<Hold> holds;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--system") system_dir = next();
        else if (a == "--save") save_dir = next();
        else if (a == "--game") game = next();
        else if (a == "--frames") frames = std::atol(next().c_str());
        else if (a == "--out") out = next();
        else if (a == "--realtime") realtime = true;
        else if (a == "--net-host") { netplay = ds::engine::Engine::Netplay{}; netplay->host = true; }
        else if (a == "--net-join") { netplay = ds::engine::Engine::Netplay{}; netplay->host = false; netplay->peer = next(); }
        else if (a == "--net-delay") net_delay = std::atoi(next().c_str());
        else if (a == "--opt") {
            const std::string kv = next();
            const auto eq = kv.find('=');
            if (eq != std::string::npos)
                opts.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        } else if (a == "--hold" || a == "--stick") {
            // Held from frame FROM up to TO: a button, or the left stick at
            // X,Y (-32768..32767).
            const std::string v = next();
            const auto at = v.find('@'), dash = v.find('-', at == std::string::npos ? 0 : at);
            if (at == std::string::npos || dash == std::string::npos)
                continue;
            Hold h;
            h.from = std::atol(v.substr(at + 1, dash - at - 1).c_str());
            h.to   = std::atol(v.substr(dash + 1).c_str());
            if (a == "--hold") {
                h.button = std::atoi(v.substr(0, at).c_str());
            } else {
                const auto comma = v.find(',');
                h.stick = true;
                h.x = static_cast<std::int16_t>(std::atoi(v.substr(0, comma).c_str()));
                h.y = static_cast<std::int16_t>(std::atoi(v.substr(comma + 1, at - comma - 1).c_str()));
            }
            holds.push_back(h);
        } else if (a == "--press") {
            const std::string bf = next();
            const auto at = bf.find('@');
            if (at != std::string::npos)
                presses.push_back({static_cast<unsigned>(std::atoi(bf.substr(0, at).c_str())),
                                   std::atol(bf.substr(at + 1).c_str())});
        }
    }
    if (system_dir.empty()) {
        std::fprintf(stderr, "usage: engine_probe --system DIR [--game PATH] [--frames N] [--out F.png]\n");
        return 2;
    }
    if (save_dir.empty())
        save_dir = system_dir;

    EGLDisplay dpy;
    EGLContext ctx;
    if (!make_context(dpy, ctx)) {
        std::fprintf(stderr, "probe: could not create an OpenGL 4.5 core context\n");
        return 1;
    }
    std::printf("GL renderer : %s\nGL version  : %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    GLuint fbo = 0, color = 0, depth = 0;
    unsigned last_w = 0, last_h = 0;
    long fresh_frames = 0;
    std::size_t audio_frames = 0;
    long audio_nonzero = 0;

    ds::engine::HostCallbacks cb;
    cb.current_framebuffer = [&] { return static_cast<std::uintptr_t>(fbo); };
    cb.get_proc_address    = egl_proc;
    cb.on_frame = [&](unsigned w, unsigned h, bool fresh) {
        if (fresh) { ++fresh_frames; last_w = w; last_h = h; }
    };
    cb.on_audio = [&](const std::int16_t* s, std::size_t n) {
        audio_frames += n;
        for (std::size_t i = 0; i < n * 2; ++i)
            if (s[i] != 0) { ++audio_nonzero; break; }
    };
    cb.on_log = [](ds::engine::LogLevel lvl, const char* line) {
        if (lvl >= ds::engine::LogLevel::Warn || std::getenv("PROBE_VERBOSE"))
            std::fprintf(stderr, "[engine] %s\n", line);
    };
    cb.on_message = [](const char* t) { std::fprintf(stderr, "[engine msg] %s\n", t); };

    Engine& e = Engine::instance();
    for (const auto& [k, v] : opts)
        e.set_option(k, v);
    e.init({system_dir, save_dir}, cb);
    if (std::getenv("PROBE_LIST_OPTIONS")) {
        for (const auto& o : e.options())
            std::printf("%-12s %-34s def=%-12s n=%zu  %s\n", o.category.c_str(), o.key.c_str(),
                        o.default_value.c_str(), o.values.size(), o.label.c_str());
        return 0;
    }
    std::fprintf(stderr, "probe: init ok\n");

    if (netplay) {
        netplay->delay = net_delay;
        e.set_netplay(netplay);
    }
    if (!e.load_game(game)) {
        std::fprintf(stderr, "probe: load_game FAILED\n");
        return 1;
    }
    const unsigned fw = e.max_width(), fh = e.max_height();
    std::printf("AV info     : max %ux%u, %.3f fps, %.0f Hz audio, aspect %.3f\n",
                fw, fh, e.refresh_rate(), e.sample_rate(), e.aspect_ratio());

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &color);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, fw, fh);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, fw, fh);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "probe: framebuffer incomplete\n");
        return 1;
    }
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    e.context_reset();

    // Netplay: wait for the other side before counting frames.
    if (netplay) {
        const auto wait0 = std::chrono::steady_clock::now();
        std::string message;
        auto state = e.netplay_state(&message);
        while (state == ds::engine::Engine::NetplayState::Connecting &&
               std::chrono::steady_clock::now() - wait0 < std::chrono::seconds(60)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            state = e.netplay_state(&message);
        }
        std::printf("Netplay     : %s after %.1fs %s\n",
                    state == ds::engine::Engine::NetplayState::Running ? "running" : "NOT running",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - wait0).count(),
                    message.c_str());
        if (state != ds::engine::Engine::NetplayState::Running)
            return 1;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const auto frame_time = std::chrono::duration<double>(1.0 / e.refresh_rate());
    long ran = 0;
    for (; ran < frames && !e.shutdown_requested(); ++ran) {
        std::uint16_t buttons = 0;
        for (const Press& p : presses)
            if (ran >= p.frame && ran < p.frame + 6)
                buttons |= static_cast<std::uint16_t>(1u << p.button);
        std::int16_t lx = 0, ly = 0;
        for (const Hold& h : holds) {
            if (ran < h.from || ran >= h.to)
                continue;
            if (h.stick) {
                lx = h.x;
                ly = h.y;
            } else {
                buttons |= static_cast<std::uint16_t>(1u << h.button);
            }
        }
        e.pad(0).buttons = buttons;
        e.pad(0).lx = lx;
        e.pad(0).ly = ly;
        e.run_frame();
        if (ran == 0)
            if (const auto arcade = e.arcade_inputs()) {
                std::printf("Arcade keys :");
                for (unsigned b = 0; b < arcade->buttons.size(); ++b)
                    if (!arcade->buttons[b].empty())
                        std::printf(" %u=%s", b, arcade->buttons[b].c_str());
                for (unsigned a = 0; a < arcade->axes.size(); ++a)
                    if (!arcade->axes[a].empty())
                        std::printf(" axis%u=%s", a, arcade->axes[a].c_str());
                std::printf("\n");
            }
        if (realtime)
            std::this_thread::sleep_until(t0 + frame_time * (ran + 1));
    }
    glFinish();   // include queued GPU work in the timing
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    int rc = 0;
    // Saves the last presented frame; false if the engine never presented one.
    const auto save_frame = [&](const std::string& file) {
        if (last_w == 0 || last_h == 0)
            return false;
        std::vector<unsigned char> px(static_cast<std::size_t>(last_w) * last_h * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, last_w, last_h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        std::size_t lit = 0;
        for (std::size_t i = 0; i < px.size(); i += 4) {
            if (px[i] > 8 || px[i + 1] > 8 || px[i + 2] > 8) ++lit;
            px[i + 3] = 255;
        }
        stbi_flip_vertically_on_write(e.bottom_left_origin() ? 1 : 0);
        stbi_write_png(file.c_str(), last_w, last_h, 4, px.data(), last_w * 4);
        std::printf("Last frame  : %ux%u, %.1f%% non-black pixels -> %s\n", last_w, last_h,
                    100.0 * lit / (px.size() / 4), file.c_str());
        return true;
    };
    if (!save_frame(out)) {
        std::fprintf(stderr, "probe: engine never presented a frame\n");
        rc = 1;
    }
    std::printf("Aspect now  : %.3f (engine-reported, after running)\n", e.aspect_ratio());
    std::printf("Calls       : %ld run_frame in %.2fs (%.1f/s)\n", ran, secs, ran / secs);
    std::printf("Presented   : %ld fresh frames (%.1f/s)\n", fresh_frames, fresh_frames / secs);
    std::printf("Audio       : %zu sample frames, %ld non-silent batches\n", audio_frames, audio_nonzero);

    if (const char* reload = std::getenv("PROBE_RELOAD")) {
        // Further sessions in the same process, as the library UI does when
        // the player switches games. Games are separated by '|'; each runs
        // --frames frames and saves its last frame as <out>.<n>.png.
        std::string list = reload;
        int n = 0;
        for (std::size_t pos = 0; pos <= list.size();) {
            const std::size_t end = std::min(list.find('|', pos), list.size());
            const std::string next = list.substr(pos, end - pos);
            pos = end + 1;
            if (next.empty())
                continue;
            e.context_destroy();
            e.unload_game();
            const long before = fresh_frames;
            last_w = last_h = 0;
            const bool ok = e.load_game(next);
            if (ok) {
                e.context_reset();
                for (long i = 0; i < frames && !e.shutdown_requested(); ++i)
                    e.run_frame();
                glFinish();
            }
            std::printf("Reload %d    : %s: load %s, %ld fresh frames\n", ++n, next.c_str(),
                        ok ? "ok" : "FAILED", fresh_frames - before);
            if (ok)
                save_frame(out + "." + std::to_string(n) + ".png");
            if (!ok || fresh_frames == before)
                rc = 1;
        }
    }
    e.context_destroy();
    e.shutdown();
    return rc;
}
