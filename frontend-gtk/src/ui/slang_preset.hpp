// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <epoxy/gl.h>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../post_types.hpp"

namespace ds::frontend {

// A RetroArch "slang" shader preset (.slangp): one or more passes, each a
// .slang file holding a vertex and a fragment stage written in Vulkan-flavoured
// GLSL, plus optional lookup textures and tunable parameters.
//
// We run them on OpenGL 4.5+ without a cross-compiler: the sources are
// rewritten just enough to be valid desktop GLSL (Vulkan-only layout
// qualifiers removed, the push-constant block turned into a second uniform
// block), and the uniform blocks are filled by looking up each member the
// shader actually uses. Supported inputs: Source, Original, PassOutputN,
// PassFeedbackN, pass aliases and their feedback, and preset textures.
class SlangPreset {
public:
    ~SlangPreset();

    // Parses the preset and its shaders (no GL needed). False on failure;
    // `error` says why.
    bool load(const std::filesystem::path& preset, std::string& error);
    // The preset's tunable parameters, in declaration order.
    const std::vector<PostOption>& parameters() const { return m_parameters; }
    void set_parameter(const std::string& name, float value) { m_values[name] = value; }

    // GL: compile every pass. False on failure, with compiler output in `error`.
    bool build(std::string& error);
    void release();

    struct Frame {
        GLuint input = 0;                    // image entering the preset
        int    input_w = 0, input_h = 0;
        int    x = 0, y = 0, w = 0, h = 0;   // where the result goes
        // Binds the destination framebuffer for the final pass.
        std::function<void()> bind_output;
        unsigned frame_count = 0;
    };
    void render(const Frame& frame);

    // Parameters only (for the settings UI): parse without keeping the object.
    static std::vector<PostOption> read_parameters(const std::filesystem::path& preset);

private:
    struct Texture {
        std::string name;
        std::filesystem::path file;
        bool linear = true, mipmap = false;
        std::string wrap = "clamp_to_border";
        GLuint id = 0;
        int w = 0, h = 0;
    };
    struct Member {                 // one used member of a uniform block
        std::string name;
        GLint  offset = 0;
        GLenum type = 0;
    };
    struct Sampler {
        std::string name;
        GLint location = -1;
    };
    struct Pass {
        std::filesystem::path file;
        std::string vertex, fragment;       // rewritten sources
        std::string alias;
        std::string scale_type_x = "", scale_type_y = "";
        float scale_x = 1.f, scale_y = 1.f;
        bool  filter_linear = false, mipmap_input = false;
        bool  float_fb = false, srgb_fb = false;
        std::string wrap = "clamp_to_border";
        std::string format;                 // from #pragma format
        unsigned frame_count_mod = 0;

        GLuint program = 0;
        GLuint ubo[2] = {0, 0};             // [0] UBO, [1] Push
        std::vector<Member>  members[2];
        std::vector<char>    block_data[2];
        std::vector<Sampler> samplers;

        // Output. Two targets when another pass reads this one's previous frame.
        GLuint fbo[2] = {0, 0}, tex[2] = {0, 0};
        int    w = 0, h = 0;
        bool   feedback = false;
        int    current = 0;
        bool   to_screen = false;
    };

    bool parse_preset(const std::filesystem::path& file, std::map<std::string, std::string>& kv,
                      std::string& error, int depth = 0);
    bool load_shader(Pass& pass, std::string& error);
    void ensure_target(Pass& pass, int w, int h);
    GLuint sampler_object(bool linear, bool mipmap, const std::string& wrap);
    void fill_blocks(Pass& pass, std::size_t index, const Frame& frame, int in_w, int in_h);
    void bind_textures(Pass& pass, std::size_t index, const Frame& frame, GLuint source,
                       bool source_linear, bool source_mipmap);

    std::vector<Pass>       m_passes;
    std::vector<Texture>    m_textures;
    std::vector<PostOption> m_parameters;
    std::map<std::string, float> m_values;
    std::map<std::string, GLuint> m_sampler_objects;
    GLuint m_vao = 0, m_vbo = 0;
    GLuint m_blit = 0;
    bool   m_built = false;
};

} // namespace ds::frontend
