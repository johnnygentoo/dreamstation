// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <epoxy/gl.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../post_types.hpp"

namespace reshadefx { struct effect_module; }

namespace ds::frontend {

// One technique of a ReShade effect (.fx), run on OpenGL.
//
// The effect is compiled with ReShade's own compiler (third_party/reshadefx)
// to GLSL, once per output size since effects are written against
// BUFFER_WIDTH/HEIGHT. Images are kept top row first, as ReShade expects, by
// flipping the vertex positions (as ReShade's OpenGL path does) and flipping
// the image on the way in and out.
//
// What the game cannot give: there is no depth buffer (effects see a blank
// one), and keyboard/mouse uniforms stay 0.
class ReshadeEffect {
public:
    ReshadeEffect();
    ~ReshadeEffect();

    // Compiles the effect for its parameters (no GL needed). False on
    // failure; `error` says why. An empty `technique` picks the first one.
    bool load(const std::filesystem::path& file, const std::string& technique, std::string& error);
    const std::vector<PostOption>& parameters() const { return m_parameters; }
    void set_parameter(const std::string& name, float value) { m_values[name] = value; }

    struct Frame {
        GLuint input = 0;                    // image entering the effect (bottom row first)
        int    input_w = 0, input_h = 0;
        int    x = 0, y = 0, w = 0, h = 0;   // where the result goes
        std::function<void()> bind_output;   // binds the destination framebuffer
        unsigned frame_count = 0;
    };
    // GL. Builds the effect for this size first if needed. False when the
    // effect cannot run (error() says why); the image is then passed through.
    bool render(const Frame& frame);
    const std::string& error() const { return m_error; }
    void release();

    // Parameters only (for the settings UI).
    static std::vector<PostOption> read_parameters(const std::filesystem::path& file,
                                                   const std::string& technique);
    // Techniques declared in a file, from a quick look at its text (for the
    // shader list; no compile).
    static std::vector<std::string> techniques(const std::filesystem::path& file);
    // Where ReShade shader packs are looked for.
    static std::vector<std::filesystem::path> roots();

private:
    struct Texture {
        std::string unique_name, semantic;
        GLenum target = GL_TEXTURE_2D, format = GL_RGBA8;
        int    w = 1, h = 1, d = 1, levels = 1;
        GLuint id = 0, srgb_view = 0;
        bool   render_target = false;
    };
    struct Pass;
    struct Uniform;
    // Textures of one effect file at one size, shared by every stage that
    // runs a technique of that file: paired techniques (Before/After,
    // Start/End) hand images to each other through them, as in ReShade.
    struct Shared {
        std::map<std::string, Texture> textures;   // by unique name
        ~Shared();
    };
    std::shared_ptr<Shared> m_shared;

    bool compile(int width, int height, std::string& error);
    bool build(std::string& error);
    void destroy_gl();
    void update_uniforms(float frame_ms);
    void run_pass(const Pass& pass);
    GLuint texture_for(const std::string& unique_name, bool srgb);
    GLuint stencil_for(int w, int h);

    std::filesystem::path m_file;
    std::string           m_technique;
    std::vector<PostOption>      m_parameters;
    std::map<std::string, float> m_values;
    std::string m_error;

    // From the last compile.
    std::unique_ptr<reshadefx::effect_module> m_module;
    std::map<std::string, std::string> m_code;   // entry point -> GLSL
    std::vector<Uniform> m_uniforms;
    int m_compiled_w = 0, m_compiled_h = 0;

    // GL objects for the compiled size.
    bool   m_built = false, m_failed = false;
    std::vector<Texture> m_textures;
    std::vector<GLuint>  m_samplers;             // per module sampler
    std::vector<std::unique_ptr<Pass>> m_passes;
    GLuint m_ubo = 0, m_vao = 0;
    GLuint m_fbo = 0, m_read_fbo = 0;
    GLuint m_back[2] = {0, 0}, m_back_srgb[2] = {0, 0};
    int    m_cur = 0;
    GLuint m_stencil = 0;
    int    m_stencil_w = 0, m_stencil_h = 0;
    GLuint m_blank_depth = 0;
    std::vector<unsigned char> m_ubo_data;

    // A new output size must hold for a few frames before rebuilding, so
    // resizing the window does not recompile on every step.
    int m_pending_w = 0, m_pending_h = 0, m_pending_frames = 0;

    std::chrono::steady_clock::time_point m_start, m_last;
    unsigned m_frame_count = 0;
    bool     m_stencil_cleared = false;
};

} // namespace ds::frontend
