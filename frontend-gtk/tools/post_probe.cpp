// SPDX-License-Identifier: GPL-3.0-or-later
//
// post_probe — runs the post-processing chain headless on a picture, so a
// shader's result can be looked at without starting the app.
//
//   post_probe --in IMAGE --out FILE.png --stage NAME [--stage NAME]...
//              [--opt STAGE_INDEX:name=value]... [--size WxH] [--frames N] [--realtime]
//   post_probe --list [FILTER]        shaders the library offers
//   post_probe --params NAME          a shader's options
//
// Shaders are found where the app looks (XDG_DATA_HOME/dreamstation/shaders...).
#include "../src/ui/post_chain.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <gdkmm/pixbuf.h>
#include <gdkmm/wrap_init.h>
#include <giomm/init.h>
#include <glibmm/init.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace ds::frontend;

namespace {

bool make_context() {
    EGLDisplay dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr))
        return false;
    eglBindAPI(EGL_OPENGL_API);
    const EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n == 0)
        return false;
    const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                               EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    return ctx != EGL_NO_CONTEXT && eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
}

} // namespace

int main(int argc, char** argv) {
    Glib::init();
    Gio::init();
    Gdk::wrap_init();
    std::string in, out = "post.png";
    std::vector<PostStage> stages;
    int width = 0, height = 0, frames = 3;
    bool realtime = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--in") in = next();
        else if (a == "--out") out = next();
        else if (a == "--stage") stages.push_back({next(), true, {}});
        else if (a == "--frames") frames = std::atoi(next().c_str());
        else if (a == "--realtime") realtime = true;     // 60 frames a second
        else if (a == "--size") {
            const std::string s = next();
            width  = std::atoi(s.c_str());
            height = std::atoi(s.substr(s.find('x') + 1).c_str());
        } else if (a == "--opt") {
            const std::string s = next();
            const auto colon = s.find(':'), eq = s.find('=');
            const auto index = static_cast<std::size_t>(std::atoi(s.substr(0, colon).c_str()));
            if (index < stages.size() && eq != std::string::npos)
                stages[index].options[s.substr(colon + 1, eq - colon - 1)] =
                    std::strtof(s.substr(eq + 1).c_str(), nullptr);
        } else if (a == "--list") {
            const std::string filter = i + 1 < argc ? argv[++i] : "";
            for (const auto& name : PostLibrary::available())
                if (name.find(filter) != std::string::npos)
                    std::printf("%s\n", name.c_str());
            return 0;
        } else if (a == "--params") {
            for (const auto& o : PostLibrary::options(next())) {
                std::printf("%s  \"%s\"  %s  def %g  [%g, %g] step %g", o.name.c_str(), o.label.c_str(),
                            o.type == PostOption::Type::Bool ? "bool" : o.type == PostOption::Type::Int ? "int" : "float",
                            static_cast<double>(o.def), static_cast<double>(o.min),
                            static_cast<double>(o.max), static_cast<double>(o.step));
                for (const auto& item : o.items)
                    std::printf(" |%s", item.c_str());
                std::printf("\n");
            }
            return 0;
        }
    }
    if (in.empty() || stages.empty()) {
        std::fprintf(stderr, "usage: post_probe --in IMAGE --out FILE.png --stage NAME...\n");
        return 2;
    }
    if (!make_context()) {
        std::fprintf(stderr, "post_probe: no OpenGL 4.5 context\n");
        return 1;
    }

    auto picture = Gdk::Pixbuf::create_from_file(in);
    if (!picture->get_has_alpha())
        picture = picture->add_alpha(false, 0, 0, 0);
    const int in_w = picture->get_width(), in_h = picture->get_height();
    if (width <= 0) { width = in_w; height = in_h; }
    // The chain takes the game's image bottom row first, as GL stores it.
    auto flipped = picture->flip(false);
    GLuint source = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &source);
    glTextureStorage2D(source, 1, GL_RGBA8, in_w, in_h);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, flipped->get_rowstride() / 4);
    glTextureSubImage2D(source, 0, 0, 0, in_w, in_h, GL_RGBA, GL_UNSIGNED_BYTE, flipped->get_pixels());
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    GLuint target = 0, fbo = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &target);
    glTextureStorage2D(target, 1, GL_RGBA8, width, height);
    glCreateFramebuffers(1, &fbo);
    glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, target, 0);

    PostChain chain;
    chain.set_stages(stages);
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) {
        PostChain::Frame frame;
        frame.source = source;
        frame.source_w = in_w;
        frame.source_h = in_h;
        frame.w = width;
        frame.h = height;
        frame.time = static_cast<float>(f) / 60.f;
        chain.render(frame, [fbo] { glBindFramebuffer(GL_FRAMEBUFFER, fbo); });
        if (realtime) {
            glFinish();
            std::this_thread::sleep_until(t0 + std::chrono::microseconds(16667) * (f + 1));
        }
    }
    glFinish();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const GLenum gl_error = glGetError();

    std::vector<unsigned char> px(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (std::size_t i = 3; i < px.size(); i += 4)
        px[i] = 255;
    auto result = Gdk::Pixbuf::create_from_data(px.data(), Gdk::Colorspace::RGB, true, 8, width, height, width * 4);
    result->flip(false)->save(out, "png");

    std::printf("frames %d in %.1f ms, GL error 0x%x -> %s\n", frames, ms, gl_error, out.c_str());
    if (!chain.errors().empty()) {
        std::printf("errors:\n%s", chain.errors().c_str());
        return 1;
    }
    return 0;
}
