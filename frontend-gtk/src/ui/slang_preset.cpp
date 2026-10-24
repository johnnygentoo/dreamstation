// SPDX-License-Identifier: GPL-3.0-or-later
#include "slang_preset.hpp"
#include "../image_io.hpp"
#include <dreamstation/common/log.hpp>

#include <gdkmm/pixbuf.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace ds::frontend {

namespace fs = std::filesystem;

namespace {

std::string trim(std::string s) {
    const auto ws = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!s.empty() && ws(s.back())) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && ws(s[i])) ++i;
    return s.substr(i);
}

std::string unquote(std::string s) {
    s = trim(std::move(s));
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = s.substr(1, s.size() - 2);
    return s;
}

bool truthy(const std::string& v) { return v == "true" || v == "1"; }

// Read a shader file with its #include lines expanded in place.
bool read_with_includes(const fs::path& file, std::string& out, std::string& error, int depth = 0) {
    if (depth > 16) {
        error = "includes nested too deeply at " + file.string();
        return false;
    }
    std::ifstream in(file);
    if (!in) {
        error = "cannot open " + file.string();
        return false;
    }
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::string t = trim(line);
        if (t.rfind("#include", 0) == 0) {
            const auto a = t.find('"'), b = t.rfind('"');
            if (a == std::string::npos || b <= a) {
                error = "bad #include in " + file.string();
                return false;
            }
            if (!read_with_includes(file.parent_path() / t.substr(a + 1, b - a - 1), out, error,
                                    depth + 1))
                return false;
            continue;
        }
        out += line;
        out += '\n';
    }
    return true;
}

// Make Vulkan-flavoured layout qualifiers valid for desktop OpenGL: drop the
// descriptor set, and turn the push-constant block into a uniform block at
// binding point 1.
std::string rewrite_layouts(const std::string& src) {
    static const std::regex layout_re(R"(layout\s*\(([^)]*)\))");
    std::string out;
    auto begin = std::sregex_iterator(src.begin(), src.end(), layout_re);
    std::size_t last = 0;
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        out.append(src, last, static_cast<std::size_t>(it->position()) - last);
        last = static_cast<std::size_t>(it->position() + it->length());
        std::vector<std::string> kept;
        std::istringstream items((*it)[1].str());
        for (std::string item; std::getline(items, item, ',');) {
            item = trim(item);
            if (item.rfind("set", 0) == 0 && item.find('=') != std::string::npos)
                continue;
            if (item == "push_constant") {
                kept.push_back("std140");
                kept.push_back("binding = 1");
                continue;
            }
            if (!item.empty())
                kept.push_back(item);
        }
        if (kept.empty())
            continue;
        out += "layout(";
        for (std::size_t i = 0; i < kept.size(); ++i)
            out += (i ? ", " : "") + kept[i];
        out += ")";
    }
    out.append(src, last, std::string::npos);
    return out;
}

GLuint compile_stage(GLenum type, const std::string& source, std::string& log) {
    const GLuint shader = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[4096];
        glGetShaderInfoLog(shader, sizeof(buf), nullptr, buf);
        log = buf;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint link_program(const std::string& vs_src, const std::string& fs_src, std::string& log) {
    const GLuint vs = compile_stage(GL_VERTEX_SHADER, vs_src, log);
    if (!vs) { log = "vertex stage: " + log; return 0; }
    const GLuint fsh = compile_stage(GL_FRAGMENT_SHADER, fs_src, log);
    if (!fsh) { glDeleteShader(vs); log = "fragment stage: " + log; return 0; }
    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fsh);
    glBindAttribLocation(program, 0, "Position");
    glBindAttribLocation(program, 1, "TexCoord");
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fsh);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[2048];
        glGetProgramInfoLog(program, sizeof(buf), nullptr, buf);
        log = std::string("link: ") + buf;
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

GLenum internal_format(const std::string& format, bool float_fb, bool srgb_fb) {
    if (format == "R8G8B8A8_SRGB")            return GL_SRGB8_ALPHA8;
    if (format == "R16G16B16A16_SFLOAT")      return GL_RGBA16F;
    if (format == "R32G32B32A32_SFLOAT")      return GL_RGBA32F;
    if (format == "A2B10G10R10_UNORM_PACK32") return GL_RGB10_A2;
    if (format == "R16G16B16A16_UNORM")       return GL_RGBA16;
    if (format == "R8G8B8A8_UNORM")           return GL_RGBA8;
    if (float_fb) return GL_RGBA16F;
    if (srgb_fb)  return GL_SRGB8_ALPHA8;
    return GL_RGBA8;
}

const char* const kBlitVertex = R"glsl(#version 450 core
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
out vec2 uv;
void main() { gl_Position = vec4(Position.xy * 2.0 - 1.0, 0.0, 1.0); uv = TexCoord; }
)glsl";
const char* const kBlitFragment = R"glsl(#version 450 core
in vec2 uv;
out vec4 color;
uniform sampler2D tex;
void main() { color = vec4(texture(tex, uv).rgb, 1.0); }
)glsl";

// "Name3" -> ("Name", 3); returns false if there is no trailing number.
bool split_index(const std::string& s, const std::string& prefix, std::size_t& index) {
    if (s.size() <= prefix.size() || s.compare(0, prefix.size(), prefix) != 0)
        return false;
    const std::string digits = s.substr(prefix.size());
    if (!std::all_of(digits.begin(), digits.end(),
                     [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
        return false;
    index = static_cast<std::size_t>(std::stoul(digits));
    return true;
}

} // namespace

SlangPreset::~SlangPreset() = default;   // GL objects are freed by release()

// ── Preset parsing ────────────────────────────────────────────────────────

// Values are stored as written; paths are stored alongside under "@dir:<key>"
// so they can be resolved against the preset file that mentioned them.
bool SlangPreset::parse_preset(const fs::path& file, std::map<std::string, std::string>& kv,
                               std::string& error, int depth) {
    if (depth > 8) {
        error = "#reference nested too deeply";
        return false;
    }
    std::ifstream in(file);
    if (!in) {
        error = "cannot open " + file.string();
        return false;
    }
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.rfind("#reference", 0) == 0) {
            const fs::path ref = file.parent_path() / unquote(line.substr(10));
            if (!parse_preset(ref, kv, error, depth + 1))
                return false;
            continue;
        }
        if (line.empty() || line[0] == '#' || line.rfind("//", 0) == 0)
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, eq));
        std::string value = line.substr(eq + 1);
        // Strip a trailing comment outside quotes.
        bool quoted = false;
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '"') quoted = !quoted;
            if (!quoted && value[i] == '#') { value.erase(i); break; }
        }
        kv[key] = unquote(value);
        kv["@dir:" + key] = file.parent_path().string();
    }
    return true;
}

bool SlangPreset::load_shader(Pass& pass, std::string& error) {
    std::string text;
    if (!read_with_includes(pass.file, text, error))
        return false;

    std::string common, vertex, fragment;
    std::string* section = &common;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        const std::string t = trim(line);
        if (t.rfind("#version", 0) == 0)
            continue;
        if (t.rfind("#pragma", 0) == 0) {
            std::istringstream words(t.substr(7));
            std::string what;
            words >> what;
            if (what == "stage") {
                std::string stage;
                words >> stage;
                section = stage == "vertex" ? &vertex : &fragment;
                // Keep line numbers roughly meaningful in compiler messages.
                *section += "\n";
                continue;
            }
            if (what == "name") {
                std::string name;
                words >> name;
                if (pass.alias.empty()) pass.alias = name;
                continue;
            }
            if (what == "format") {
                words >> pass.format;
                continue;
            }
            if (what == "parameter") {
                // #pragma parameter NAME "Description" default min max [step]
                PostOption opt;
                words >> opt.name;
                std::string rest;
                std::getline(words, rest);
                const auto a = rest.find('"'), b = rest.rfind('"');
                if (a != std::string::npos && b > a) {
                    opt.label = rest.substr(a + 1, b - a - 1);
                    std::istringstream nums(rest.substr(b + 1));
                    nums >> opt.def >> opt.min >> opt.max;
                    if (!(nums >> opt.step) || opt.step <= 0.f)
                        opt.step = (opt.max - opt.min) / 100.f;
                    opt.type = PostOption::Type::Float;
                    if (opt.label.empty()) opt.label = opt.name;
                    const bool known = std::any_of(m_parameters.begin(), m_parameters.end(),
                        [&](const PostOption& o) { return o.name == opt.name; });
                    if (!known && !opt.name.empty())
                        m_parameters.push_back(opt);
                }
                continue;
            }
        }
        *section += line;
        *section += '\n';
    }
    if (vertex.empty() || fragment.empty()) {
        error = pass.file.filename().string() + ": missing a vertex or fragment stage";
        return false;
    }
    const std::string head = "#version 450 core\n";
    pass.vertex   = rewrite_layouts(head + common + vertex);
    pass.fragment = rewrite_layouts(head + common + fragment);
    return true;
}

bool SlangPreset::load(const fs::path& preset, std::string& error) {
    m_passes.clear();
    m_textures.clear();
    m_parameters.clear();

    std::map<std::string, std::string> kv;
    if (!parse_preset(preset, kv, error))
        return false;
    const auto get = [&](const std::string& key, const std::string& fallback = {}) {
        const auto it = kv.find(key);
        return it == kv.end() ? fallback : it->second;
    };
    const auto path_of = [&](const std::string& key) {
        return fs::path(get("@dir:" + key)) / get(key);
    };

    const int count = std::atoi(get("shaders", "0").c_str());
    if (count <= 0 || count > 64) {
        error = "preset declares no shaders";
        return false;
    }
    for (int i = 0; i < count; ++i) {
        const std::string n = std::to_string(i);
        if (get("shader" + n).empty()) {
            error = "preset is missing shader" + n;
            return false;
        }
        Pass pass;
        pass.file          = path_of("shader" + n);
        pass.alias         = get("alias" + n);
        pass.filter_linear = truthy(get("filter_linear" + n, "false"));
        pass.mipmap_input  = truthy(get("mipmap_input" + n, "false"));
        pass.float_fb      = truthy(get("float_framebuffer" + n, "false"));
        pass.srgb_fb       = truthy(get("srgb_framebuffer" + n, "false"));
        pass.wrap          = get("wrap_mode" + n, "clamp_to_border");
        pass.frame_count_mod = static_cast<unsigned>(std::atoi(get("frame_count_mod" + n, "0").c_str()));
        const std::string type = get("scale_type" + n);
        pass.scale_type_x = get("scale_type_x" + n, type);
        pass.scale_type_y = get("scale_type_y" + n, type);
        const std::string scale = get("scale" + n, "1.0");
        pass.scale_x = std::strtof(get("scale_x" + n, scale).c_str(), nullptr);
        pass.scale_y = std::strtof(get("scale_y" + n, scale).c_str(), nullptr);
        if (!load_shader(pass, error))
            return false;
        m_passes.push_back(std::move(pass));
    }

    std::istringstream names(get("textures"));
    for (std::string name; std::getline(names, name, ';');) {
        name = trim(name);
        if (name.empty() || get(name).empty())
            continue;
        Texture t;
        t.name   = name;
        t.file   = path_of(name);
        t.linear = truthy(get(name + "_linear", "true"));
        t.mipmap = truthy(get(name + "_mipmap", "false"));
        t.wrap   = get(name + "_wrap_mode", "clamp_to_border");
        m_textures.push_back(std::move(t));
    }

    // The preset may override parameter defaults.
    for (auto& p : m_parameters) {
        const auto it = kv.find(p.name);
        if (it != kv.end() && !it->second.empty())
            p.def = std::strtof(it->second.c_str(), nullptr);
    }
    return true;
}

std::vector<PostOption> SlangPreset::read_parameters(const fs::path& preset) {
    SlangPreset p;
    std::string error;
    if (!p.load(preset, error))
        return {};
    return p.m_parameters;
}

// ── GL objects ────────────────────────────────────────────────────────────

bool SlangPreset::build(std::string& error) {
    release();

    static const float quad[] = {  // position.xy, texcoord.xy
        0.f, 0.f, 0.f, 0.f,   1.f, 0.f, 1.f, 0.f,   0.f, 1.f, 0.f, 1.f,   1.f, 1.f, 1.f, 1.f};
    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glBindVertexArray(0);

    std::string log;
    m_blit = link_program(kBlitVertex, kBlitFragment, log);

    for (std::size_t i = 0; i < m_passes.size(); ++i) {
        Pass& pass = m_passes[i];
        pass.program = link_program(pass.vertex, pass.fragment, log);
        if (!pass.program) {
            error = "pass " + std::to_string(i) + " (" + pass.file.filename().string() + "): " + log;
            release();
            return false;
        }

        // Uniform blocks: which binding point (0 = UBO, 1 = Push) and how big.
        GLint blocks = 0;
        glGetProgramiv(pass.program, GL_ACTIVE_UNIFORM_BLOCKS, &blocks);
        std::map<GLint, int> slot_of_block;
        for (GLint b = 0; b < blocks; ++b) {
            GLint binding = 0, size = 0;
            glGetActiveUniformBlockiv(pass.program, static_cast<GLuint>(b), GL_UNIFORM_BLOCK_BINDING, &binding);
            glGetActiveUniformBlockiv(pass.program, static_cast<GLuint>(b), GL_UNIFORM_BLOCK_DATA_SIZE, &size);
            const int slot = binding == 1 ? 1 : 0;
            slot_of_block[b] = slot;
            pass.block_data[slot].assign(static_cast<std::size_t>(size), 0);
            glGenBuffers(1, &pass.ubo[slot]);
            glBindBuffer(GL_UNIFORM_BUFFER, pass.ubo[slot]);
            glBufferData(GL_UNIFORM_BUFFER, size, nullptr, GL_DYNAMIC_DRAW);
        }
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

        GLint uniforms = 0;
        glGetProgramiv(pass.program, GL_ACTIVE_UNIFORMS, &uniforms);
        for (GLint u = 0; u < uniforms; ++u) {
            const GLuint index = static_cast<GLuint>(u);
            char name_buf[256];
            GLsizei len = 0;
            glGetActiveUniformName(pass.program, index, sizeof(name_buf), &len, name_buf);
            std::string name(name_buf, static_cast<std::size_t>(len));
            GLint block = -1, offset = 0, type = 0;
            glGetActiveUniformsiv(pass.program, 1, &index, GL_UNIFORM_BLOCK_INDEX, &block);
            glGetActiveUniformsiv(pass.program, 1, &index, GL_UNIFORM_OFFSET, &offset);
            glGetActiveUniformsiv(pass.program, 1, &index, GL_UNIFORM_TYPE, &type);
            if (block >= 0) {
                if (const auto dot = name.rfind('.'); dot != std::string::npos)
                    name.erase(0, dot + 1);
                pass.members[slot_of_block[block]].push_back({name, offset, static_cast<GLenum>(type)});
            } else if (type == GL_SAMPLER_2D) {
                pass.samplers.push_back({name, glGetUniformLocation(pass.program, name.c_str())});
            }
        }
    }

    // Which passes must keep their previous frame for someone's feedback input.
    for (const Pass& reader : m_passes) {
        for (const Sampler& s : reader.samplers) {
            std::size_t index = 0;
            if (split_index(s.name, "PassFeedback", index) && index < m_passes.size())
                m_passes[index].feedback = true;
            for (Pass& p : m_passes)
                if (!p.alias.empty() && s.name == p.alias + "Feedback")
                    p.feedback = true;
        }
    }

    // The final pass draws straight to the screen unless the preset gives it
    // a size of its own.
    Pass& last = m_passes.back();
    last.to_screen = (last.scale_type_x.empty() || last.scale_type_x == "viewport") &&
                     (last.scale_type_y.empty() || last.scale_type_y == "viewport") &&
                     last.scale_x == 1.f && last.scale_y == 1.f && !last.feedback;

    // Lookup textures. Rows are flipped so they sit the right way up in
    // OpenGL's bottom-left-origin texture space.
    for (Texture& t : m_textures) {
        try {
            auto pixbuf = image::load_ref(t.file);
            if (!pixbuf)
                throw std::runtime_error("cannot read " + t.file.string());
            if (!pixbuf->get_has_alpha())
                pixbuf = pixbuf->add_alpha(false, 0, 0, 0);
            t.w = pixbuf->get_width();
            t.h = pixbuf->get_height();
            std::vector<guint8> rows(static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h) * 4);
            for (int y = 0; y < t.h; ++y)
                std::memcpy(&rows[static_cast<std::size_t>(t.h - 1 - y) * static_cast<std::size_t>(t.w) * 4],
                            pixbuf->get_pixels() + static_cast<std::ptrdiff_t>(y) * pixbuf->get_rowstride(),
                            static_cast<std::size_t>(t.w) * 4);
            glGenTextures(1, &t.id);
            glBindTexture(GL_TEXTURE_2D, t.id);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
            if (t.mipmap)
                glGenerateMipmap(GL_TEXTURE_2D);
        } catch (const std::exception& e) {   // also Glib::Error
            ds::common::log()->warn("slang: cannot load texture {}: {}", t.file.string(), e.what());
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    m_built = true;
    return true;
}

void SlangPreset::release() {
    for (Pass& p : m_passes) {
        if (p.program) glDeleteProgram(p.program);
        glDeleteBuffers(2, p.ubo);
        glDeleteFramebuffers(2, p.fbo);
        glDeleteTextures(2, p.tex);
        p.program = 0;
        p.ubo[0] = p.ubo[1] = p.fbo[0] = p.fbo[1] = p.tex[0] = p.tex[1] = 0;
        p.w = p.h = 0;
        p.members[0].clear(); p.members[1].clear();
        p.samplers.clear();
        p.feedback = false;
    }
    for (Texture& t : m_textures) {
        if (t.id) glDeleteTextures(1, &t.id);
        t.id = 0;
    }
    for (auto& [key, id] : m_sampler_objects)
        glDeleteSamplers(1, &id);
    m_sampler_objects.clear();
    if (m_blit) { glDeleteProgram(m_blit); m_blit = 0; }
    if (m_vbo)  { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
    if (m_vao)  { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    m_built = false;
}

void SlangPreset::ensure_target(Pass& pass, int w, int h) {
    w = std::clamp(w, 1, 16384);
    h = std::clamp(h, 1, 16384);
    if (pass.tex[0] && pass.w == w && pass.h == h)
        return;
    glDeleteFramebuffers(2, pass.fbo);
    glDeleteTextures(2, pass.tex);
    pass.fbo[0] = pass.fbo[1] = pass.tex[0] = pass.tex[1] = 0;
    const GLenum format = internal_format(pass.format, pass.float_fb, pass.srgb_fb);
    const int levels = 1 + static_cast<int>(std::floor(std::log2(std::max(w, h))));
    for (int i = 0; i < (pass.feedback ? 2 : 1); ++i) {
        glGenTextures(1, &pass.tex[i]);
        glBindTexture(GL_TEXTURE_2D, pass.tex[i]);
        glTexStorage2D(GL_TEXTURE_2D, levels, format, w, h);
        glGenFramebuffers(1, &pass.fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, pass.fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pass.tex[i], 0);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    pass.w = w;
    pass.h = h;
    pass.current = 0;
}

GLuint SlangPreset::sampler_object(bool linear, bool mipmap, const std::string& wrap) {
    const std::string key = std::string(linear ? "L" : "N") + (mipmap ? "M" : "-") + wrap;
    if (const auto it = m_sampler_objects.find(key); it != m_sampler_objects.end())
        return it->second;
    GLuint id = 0;
    glGenSamplers(1, &id);
    const GLint mag = linear ? GL_LINEAR : GL_NEAREST;
    const GLint min = mipmap ? (linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST) : mag;
    GLint mode = GL_CLAMP_TO_BORDER;
    if (wrap == "clamp_to_edge")        mode = GL_CLAMP_TO_EDGE;
    else if (wrap == "repeat")          mode = GL_REPEAT;
    else if (wrap == "mirrored_repeat") mode = GL_MIRRORED_REPEAT;
    glSamplerParameteri(id, GL_TEXTURE_MIN_FILTER, min);
    glSamplerParameteri(id, GL_TEXTURE_MAG_FILTER, mag);
    glSamplerParameteri(id, GL_TEXTURE_WRAP_S, mode);
    glSamplerParameteri(id, GL_TEXTURE_WRAP_T, mode);
    m_sampler_objects[key] = id;
    return id;
}

// ── Per-frame ─────────────────────────────────────────────────────────────

void SlangPreset::fill_blocks(Pass& pass, std::size_t index, const Frame& frame, int in_w, int in_h) {
    const auto size4 = [](float* out, int w, int h) {
        out[0] = static_cast<float>(w);
        out[1] = static_cast<float>(h);
        out[2] = w > 0 ? 1.f / static_cast<float>(w) : 0.f;
        out[3] = h > 0 ? 1.f / static_cast<float>(h) : 0.f;
    };
    // Maps the unit quad onto the whole render target.
    static const float mvp[16] = {2, 0, 0, 0,   0, 2, 0, 0,   0, 0, 1, 0,   -1, -1, 0, 1};
    const int out_w = pass.to_screen ? frame.w : pass.w;
    const int out_h = pass.to_screen ? frame.h : pass.h;

    for (int slot = 0; slot < 2; ++slot) {
        if (!pass.ubo[slot])
            continue;
        std::vector<char>& data = pass.block_data[slot];
        for (const Member& m : pass.members[slot]) {
            const auto fits = [&](std::size_t bytes) {
                return m.offset >= 0 && static_cast<std::size_t>(m.offset) + bytes <= data.size();
            };
            char* dst = data.data() + m.offset;
            float v4[4] = {0, 0, 0, 0};
            bool is_size = true;
            std::size_t n = 0;

            if (m.name == "MVP") {
                if (fits(sizeof(mvp))) std::memcpy(dst, mvp, sizeof(mvp));
                continue;
            } else if (m.name == "SourceSize")          size4(v4, in_w, in_h);
            else if (m.name == "OriginalSize")          size4(v4, frame.input_w, frame.input_h);
            else if (m.name == "OutputSize")            size4(v4, out_w, out_h);
            else if (m.name == "FinalViewportSize")     size4(v4, frame.w, frame.h);
            else if (m.name.rfind("OriginalHistorySize", 0) == 0) size4(v4, frame.input_w, frame.input_h);
            else if ((split_index(m.name, "PassOutputSize", n) ||
                      split_index(m.name, "PassFeedbackSize", n)) && n < m_passes.size())
                size4(v4, m_passes[n].w, m_passes[n].h);
            else
                is_size = false;

            if (!is_size) {
                // "<alias>Size" / "<alias>FeedbackSize" / "<texture>Size"
                for (const Pass& p : m_passes)
                    if (!p.alias.empty() && (m.name == p.alias + "Size" || m.name == p.alias + "FeedbackSize")) {
                        size4(v4, p.w, p.h);
                        is_size = true;
                    }
                for (const Texture& t : m_textures)
                    if (m.name == t.name + "Size") {
                        size4(v4, t.w, t.h);
                        is_size = true;
                    }
            }
            if (is_size) {
                if (fits(sizeof(v4))) std::memcpy(dst, v4, sizeof(v4));
                continue;
            }

            if (m.name == "FrameCount") {
                std::uint32_t count = frame.frame_count;
                if (pass.frame_count_mod) count %= pass.frame_count_mod;
                if (m.type == GL_FLOAT) {
                    const float f = static_cast<float>(count);
                    if (fits(4)) std::memcpy(dst, &f, 4);
                } else if (fits(4)) {
                    std::memcpy(dst, &count, 4);
                }
                continue;
            }
            if (m.name == "FrameDirection") {
                const std::int32_t one = 1;
                if (fits(4)) std::memcpy(dst, &one, 4);
                continue;
            }
            // Otherwise a tunable parameter.
            float value = 0.f;
            if (const auto it = m_values.find(m.name); it != m_values.end()) {
                value = it->second;
            } else {
                for (const PostOption& o : m_parameters)
                    if (o.name == m.name) { value = o.def; break; }
            }
            if (m.type == GL_FLOAT && fits(4))
                std::memcpy(dst, &value, 4);
        }
        glBindBuffer(GL_UNIFORM_BUFFER, pass.ubo[slot]);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(data.size()), data.data());
        glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(slot), pass.ubo[slot]);
        (void)index;
    }
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void SlangPreset::bind_textures(Pass& pass, std::size_t index, const Frame& frame, GLuint source,
                                bool source_linear, bool source_mipmap) {
    GLuint unit = 0;
    for (const Sampler& s : pass.samplers) {
        GLuint tex = 0;
        GLuint sampler = sampler_object(source_linear, false, pass.wrap);
        std::size_t n = 0;

        if (s.name == "Source") {
            tex = source;
            sampler = sampler_object(source_linear, source_mipmap, pass.wrap);
        } else if (s.name == "Original" || s.name.rfind("OriginalHistory", 0) == 0) {
            tex = frame.input;     // no frame history is kept; the current frame stands in
        } else if (split_index(s.name, "PassOutput", n) && n < m_passes.size()) {
            tex = m_passes[n].tex[m_passes[n].current];
        } else if (split_index(s.name, "PassFeedback", n) && n < m_passes.size()) {
            const Pass& p = m_passes[n];
            tex = p.feedback ? p.tex[p.current ^ 1] : p.tex[p.current];
        } else {
            for (std::size_t i = 0; i < m_passes.size(); ++i) {
                const Pass& p = m_passes[i];
                if (p.alias.empty())
                    continue;
                if (s.name == p.alias)
                    tex = p.tex[p.current];
                else if (s.name == p.alias + "Feedback")
                    tex = p.feedback ? p.tex[p.current ^ 1] : p.tex[p.current];
            }
            for (const Texture& t : m_textures)
                if (s.name == t.name) {
                    tex = t.id;
                    sampler = sampler_object(t.linear, t.mipmap, t.wrap);
                }
        }
        // A pass must not sample the target it is drawing into.
        if (!pass.to_screen && tex == pass.tex[pass.current] && tex != 0)
            tex = pass.feedback ? pass.tex[pass.current ^ 1] : 0;

        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, tex);
        glBindSampler(unit, sampler);
        if (s.location >= 0)
            glUniform1i(s.location, static_cast<GLint>(unit));
        ++unit;
        (void)index;
    }
    // Leave the units we used without sampler objects afterwards (done in render()).
}

void SlangPreset::render(const Frame& frame) {
    if (!m_built || m_passes.empty())
        return;

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(m_vao);

    GLuint source = frame.input;
    int in_w = frame.input_w, in_h = frame.input_h;
    GLuint max_units = 0;

    for (std::size_t i = 0; i < m_passes.size(); ++i) {
        Pass& pass = m_passes[i];
        const bool last = i + 1 == m_passes.size();

        const auto dimension = [&](const std::string& type, float scale, int src, int viewport) {
            if (type == "viewport") return static_cast<int>(std::lround(static_cast<float>(viewport) * scale));
            if (type == "absolute") return static_cast<int>(std::lround(scale));
            if (type == "source")   return static_cast<int>(std::lround(static_cast<float>(src) * scale));
            // Unspecified: intermediate passes keep the source size, the last fills the viewport.
            return last ? viewport : src;
        };

        if (pass.to_screen) {
            frame.bind_output();
            glDisable(GL_FRAMEBUFFER_SRGB);
            glViewport(frame.x, frame.y, frame.w, frame.h);
        } else {
            ensure_target(pass, dimension(pass.scale_type_x, pass.scale_x, in_w, frame.w),
                          dimension(pass.scale_type_y, pass.scale_y, in_h, frame.h));
            glBindFramebuffer(GL_FRAMEBUFFER, pass.fbo[pass.current]);
            const GLenum fmt = internal_format(pass.format, pass.float_fb, pass.srgb_fb);
            if (fmt == GL_SRGB8_ALPHA8) glEnable(GL_FRAMEBUFFER_SRGB);
            else                        glDisable(GL_FRAMEBUFFER_SRGB);
            glViewport(0, 0, pass.w, pass.h);
        }

        glUseProgram(pass.program);
        fill_blocks(pass, i, frame, in_w, in_h);
        bind_textures(pass, i, frame, source, pass.filter_linear, pass.mipmap_input);
        max_units = std::max<GLuint>(max_units, static_cast<GLuint>(pass.samplers.size()));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        if (!pass.to_screen) {
            // The next pass may want mipmaps of this output.
            if (!last && m_passes[i + 1].mipmap_input) {
                glBindTexture(GL_TEXTURE_2D, pass.tex[pass.current]);
                glGenerateMipmap(GL_TEXTURE_2D);
            }
            source = pass.tex[pass.current];
            in_w = pass.w;
            in_h = pass.h;
        }
    }
    glDisable(GL_FRAMEBUFFER_SRGB);

    // A final pass with its own size still has to reach the screen.
    if (!m_passes.back().to_screen && m_blit) {
        frame.bind_output();
        glViewport(frame.x, frame.y, frame.w, frame.h);
        glUseProgram(m_blit);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, source);
        glBindSampler(0, sampler_object(true, false, "clamp_to_edge"));
        glUniform1i(glGetUniformLocation(m_blit, "tex"), 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        max_units = std::max<GLuint>(max_units, 1);
    }

    // Passes that feed their own past back swap targets for the next frame.
    for (Pass& p : m_passes)
        if (p.feedback)
            p.current ^= 1;

    for (GLuint u = 0; u < max_units; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindSampler(u, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
    glUseProgram(0);
}

} // namespace ds::frontend
