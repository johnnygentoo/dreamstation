// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <epoxy/gl.h>
#include "../post_types.hpp"
#include "slang_preset.hpp"
#include "reshade_effect.hpp"
#include <memory>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ds::frontend {

// Shaders available to the chain: the built-in set, any *.glsl under
// <data>/shaders, RetroArch slang presets and ReShade effects (.fx). Files use the Dolphin/DuckStation GLSL post-shader
// dialect (a `[configuration]` comment block declaring options, and helpers
// such as SampleLocation(), GetCoordinates(), GetResolution(), SetOutput()),
// so shaders written for those emulators can be dropped in.
class PostLibrary {
public:
    static std::vector<std::string> available();
    // Source of a shader, or empty if it does not exist.
    static std::string load(const std::string& name);
    static std::vector<PostOption> options(const std::string& name);
    static std::filesystem::path user_dir();
    // Slang presets are named "slang/<path under a slang root>"; this gives
    // the .slangp file, or an empty path if `name` is not one.
    static std::filesystem::path slang_path(const std::string& name);
    static std::vector<std::filesystem::path> slang_roots();
    // ReShade effects are named "reshade/<path of the .fx under a pack
    // folder, without .fx>", plus ":<technique>" when the file has several.
    // This gives the file and technique, or an empty path.
    static std::filesystem::path reshade_path(const std::string& name, std::string& technique);
};

// The post-processing chain: an ordered list of stages, each rendering the
// previous stage's output through its shader. Needs a current GL context for
// render() and release(); everything else is plain data.
class PostChain {
public:
    void set_stages(std::vector<PostStage> stages);
    void set_enabled(bool enabled) { m_enabled = enabled; }
    // Feed shaders a 480-line version of the game image instead of the
    // upscaled one. CRT shaders draw one scanline per input line, so they
    // need this to look right at high internal resolutions.
    void set_native_input(bool on) { m_native_input = on; }
    bool enabled() const { return m_enabled; }
    // Change one option of a compiled stage without recompiling.
    void set_option(std::size_t stage, const std::string& name, float value);

    struct Frame {
        GLuint source = 0;                 // engine image
        int    source_w = 0, source_h = 0; // internal resolution
        int    x = 0, y = 0, w = 0, h = 0; // destination rectangle on screen
        float  time = 0.f;
    };
    // Draws the frame into the destination rectangle of the framebuffer that
    // `bind_output` binds.
    void render(const Frame& frame, const std::function<void()>& bind_output);
    void release();

    // Compile/link problems from the last (re)build, one per line; empty if none.
    const std::string& errors() const { return m_errors; }
    bool take_error_flag() { const bool f = m_new_errors; m_new_errors = false; return f; }

private:
    struct Compiled {
        GLuint program = 0;
        std::shared_ptr<SlangPreset> slang;      // set for slang presets
        std::shared_ptr<ReshadeEffect> reshade;  // set for ReShade effects
        bool usable() const { return program != 0 || slang != nullptr || reshade != nullptr; }
        bool   enabled = true;
        std::vector<PostOption>      options;
        std::map<std::string, float> values;
    };
    struct Target {
        GLuint fbo = 0, tex = 0;
        int    w = 0, h = 0;
    };

    void rebuild();
    void ensure_target(Target& t, int w, int h);
    void draw(const Compiled* stage, GLuint input, const Frame& frame);
    GLuint native_input(const Frame& frame, int& w, int& h);
    GLuint build_program(const std::string& name, const std::string& source,
                         const std::vector<PostOption>& options);

    std::vector<PostStage> m_stages;
    std::vector<Compiled>  m_compiled;
    Target   m_targets[2];
    GLuint   m_vao = 0;
    GLuint   m_passthrough = 0;
    GLuint   m_downsample = 0;
    Target   m_native;
    bool     m_native_input = false;
    unsigned m_frame_count = 0;
    bool     m_enabled = true;
    bool     m_dirty = true;
    bool     m_new_errors = false;
    std::string m_errors;
};

} // namespace ds::frontend
