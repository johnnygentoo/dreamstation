// SPDX-License-Identifier: GPL-3.0-or-later
#include "reshade_effect.hpp"
#include "post_chain.hpp"
#include "../image_io.hpp"

#include <effect_codegen.hpp>
#include <effect_module.hpp>
#include <effect_parser.hpp>
#include <effect_preprocessor.hpp>

#include <gdkmm/pixbuf.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>

namespace ds::frontend {

namespace fs = std::filesystem;
using namespace reshadefx;

// One uniform variable of the effect and what fills it.
struct ReshadeEffect::Uniform {
    reshadefx::uniform info;
    std::string source;                    // "timer", "random", ... ; empty otherwise
    std::vector<std::string> options;      // parameter names, one per component; empty if none
    std::vector<float> defaults;
    float ann_min = 0.f, ann_max = 0.f;
    float step[2] = {0.f, 0.f};
    float smoothing = 0.f;
    float pp_value = 0.f, pp_dir = 1.f;    // "pingpong" state
};

struct ReshadeEffect::Pass {
    reshadefx::pass info;
    GLuint program = 0;
    bool   compute = false;
};

namespace {

// ── Annotations ───────────────────────────────────────────────────────────

const annotation* find_annotation(const std::vector<annotation>& list, const std::string& name) {
    for (const auto& a : list)
        if (a.name == name)
            return &a;
    return nullptr;
}

float annotation_float(const annotation& a, unsigned i = 0) {
    if (i >= a.type.components())
        i = 0;
    switch (a.type.base) {
    case type::t_int:   return static_cast<float>(a.value.as_int[i]);
    case type::t_bool:
    case type::t_uint:  return static_cast<float>(a.value.as_uint[i]);
    case type::t_float: return a.value.as_float[i];
    default:            return 0.f;
    }
}

std::string annotation_string(const std::vector<annotation>& list, const std::string& name) {
    const annotation* a = find_annotation(list, name);
    return a && a->type.base == type::t_string ? a->value.string_data : std::string{};
}

// ── Uniform buffer layout (std140, as the compiler lays it out) ───────────

// Writes the components of element `array_index` of a uniform. Each matrix
// row and each array element starts on a 16-byte boundary.
void put_components(std::vector<unsigned char>& data, const reshadefx::uniform& u,
                    std::size_t array_index, const std::uint32_t* bits, unsigned count) {
    const auto store = [&](std::size_t word, std::uint32_t value) {
        const std::size_t at = u.offset + word * 4;
        if (at + 4 <= data.size())
            std::memcpy(data.data() + at, &value, 4);
    };
    if (u.type.is_matrix()) {
        for (unsigned row = 0, i = 0; row < u.type.rows; ++row)
            for (unsigned col = 0; col < u.type.cols && i < count; ++col, ++i)
                store(array_index * u.type.rows * 4 + row * 4 + col, bits[row * u.type.cols + col]);
    } else if (u.type.is_array()) {
        for (unsigned i = 0; i < u.type.rows && i < count; ++i)
            store(array_index * 4 + i, bits[i]);
    } else {
        for (unsigned i = 0; i < count; ++i)
            store(i, bits[i]);
    }
}

// Converts float values to the uniform's own type and writes them.
void put_floats(std::vector<unsigned char>& data, const reshadefx::uniform& u, const float* values,
                unsigned count, std::size_t array_index = 0) {
    std::uint32_t bits[16] = {};
    count = std::min(count, 16u);
    for (unsigned i = 0; i < count; ++i) {
        if (u.type.is_floating_point() || u.type.is_matrix()) {
            std::memcpy(&bits[i], &values[i], 4);
        } else if (u.type.is_boolean()) {
            bits[i] = values[i] != 0.f ? 1u : 0u;
        } else if (u.type.is_signed()) {
            const auto v = static_cast<std::int32_t>(std::lround(values[i]));
            std::memcpy(&bits[i], &v, 4);
        } else {
            bits[i] = static_cast<std::uint32_t>(std::max(0l, std::lround(values[i])));
        }
    }
    put_components(data, u, array_index, bits, count);
}

// The declared starting value (or zero), as raw words of the uniform's type.
void put_initializer(std::vector<unsigned char>& data, const reshadefx::uniform& u) {
    const std::size_t length = u.type.is_array() ? u.type.array_length : 1;
    const constant zero{};
    for (std::size_t a = 0; a < length; ++a) {
        const constant& c = !u.has_initializer_value ? zero
                          : u.type.is_array() ? (a < u.initializer_value.array_data.size()
                                                     ? u.initializer_value.array_data[a] : zero)
                          : u.initializer_value;
        put_components(data, u, a, c.as_uint, u.type.components());
    }
}

float initializer_component(const reshadefx::uniform& u, unsigned i) {
    if (!u.has_initializer_value)
        return 0.f;
    const constant& c = u.initializer_value;
    switch (u.type.base) {
    case type::t_int:   return static_cast<float>(c.as_int[i]);
    case type::t_bool:
    case type::t_uint:  return static_cast<float>(c.as_uint[i]);
    default:            return c.as_float[i];
    }
}

// ── GL conversions ────────────────────────────────────────────────────────

GLenum gl_format(texture_format f) {
    switch (f) {
    case texture_format::r8:       return GL_R8;
    case texture_format::r16f:     return GL_R16F;
    case texture_format::r16:      return GL_R16;
    case texture_format::r32f:     return GL_R32F;
    case texture_format::r32u:     return GL_R32UI;
    case texture_format::r32i:     return GL_R32I;
    case texture_format::rg8:      return GL_RG8;
    case texture_format::rg16f:    return GL_RG16F;
    case texture_format::rg16:     return GL_RG16;
    case texture_format::rg32f:    return GL_RG32F;
    case texture_format::rgba16f:  return GL_RGBA16F;
    case texture_format::rgba16:   return GL_RGBA16;
    case texture_format::rgba32f:  return GL_RGBA32F;
    case texture_format::rgba32u:  return GL_RGBA32UI;
    case texture_format::rgba32i:  return GL_RGBA32I;
    case texture_format::rgb10a2:  return GL_RGB10_A2;
    case texture_format::rg11b10f: return GL_R11F_G11F_B10F;
    default:                       return GL_RGBA8;
    }
}

GLenum gl_blend_factor(blend_factor f) {
    switch (f) {
    case blend_factor::zero:                   return GL_ZERO;
    case blend_factor::one:                    return GL_ONE;
    case blend_factor::source_color:           return GL_SRC_COLOR;
    case blend_factor::one_minus_source_color: return GL_ONE_MINUS_SRC_COLOR;
    case blend_factor::dest_color:             return GL_DST_COLOR;
    case blend_factor::one_minus_dest_color:   return GL_ONE_MINUS_DST_COLOR;
    case blend_factor::source_alpha:           return GL_SRC_ALPHA;
    case blend_factor::one_minus_source_alpha: return GL_ONE_MINUS_SRC_ALPHA;
    case blend_factor::dest_alpha:             return GL_DST_ALPHA;
    case blend_factor::one_minus_dest_alpha:   return GL_ONE_MINUS_DST_ALPHA;
    }
    return GL_ONE;
}

GLenum gl_blend_op(blend_op op) {
    switch (op) {
    case blend_op::subtract:         return GL_FUNC_SUBTRACT;
    case blend_op::reverse_subtract: return GL_FUNC_REVERSE_SUBTRACT;
    case blend_op::min:              return GL_MIN;
    case blend_op::max:              return GL_MAX;
    default:                         return GL_FUNC_ADD;
    }
}

GLenum gl_stencil_func(stencil_func f) {
    switch (f) {
    case stencil_func::never:         return GL_NEVER;
    case stencil_func::less:          return GL_LESS;
    case stencil_func::equal:         return GL_EQUAL;
    case stencil_func::less_equal:    return GL_LEQUAL;
    case stencil_func::greater:       return GL_GREATER;
    case stencil_func::not_equal:     return GL_NOTEQUAL;
    case stencil_func::greater_equal: return GL_GEQUAL;
    default:                          return GL_ALWAYS;
    }
}

GLenum gl_stencil_op(stencil_op op) {
    switch (op) {
    case stencil_op::zero:               return GL_ZERO;
    case stencil_op::replace:            return GL_REPLACE;
    case stencil_op::increment_saturate: return GL_INCR;
    case stencil_op::decrement_saturate: return GL_DECR;
    case stencil_op::invert:             return GL_INVERT;
    case stencil_op::increment:          return GL_INCR_WRAP;
    case stencil_op::decrement:          return GL_DECR_WRAP;
    default:                             return GL_KEEP;
    }
}

GLenum gl_topology(primitive_topology t) {
    switch (t) {
    case primitive_topology::point_list:     return GL_POINTS;
    case primitive_topology::line_list:      return GL_LINES;
    case primitive_topology::line_strip:     return GL_LINE_STRIP;
    case primitive_topology::triangle_strip: return GL_TRIANGLE_STRIP;
    default:                                 return GL_TRIANGLES;
    }
}

GLenum gl_wrap(texture_address_mode m) {
    switch (m) {
    case texture_address_mode::wrap:   return GL_REPEAT;
    case texture_address_mode::mirror: return GL_MIRRORED_REPEAT;
    case texture_address_mode::border: return GL_CLAMP_TO_BORDER;
    default:                           return GL_CLAMP_TO_EDGE;
    }
}

GLuint make_sampler(const reshadefx::sampler& s) {
    GLuint id = 0;
    glCreateSamplers(1, &id);
    const auto bits = static_cast<unsigned>(s.filter);
    const bool aniso    = s.filter == filter_mode::anisotropic;
    const bool min_lin  = aniso || (bits & 0x10) != 0;
    const bool mag_lin  = aniso || (bits & 0x04) != 0;
    const bool mip_lin  = aniso || (bits & 0x01) != 0;
    const GLenum min = min_lin ? (mip_lin ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                               : (mip_lin ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
    glSamplerParameteri(id, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(min));
    glSamplerParameteri(id, GL_TEXTURE_MAG_FILTER, mag_lin ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(id, GL_TEXTURE_WRAP_S, static_cast<GLint>(gl_wrap(s.address_u)));
    glSamplerParameteri(id, GL_TEXTURE_WRAP_T, static_cast<GLint>(gl_wrap(s.address_v)));
    glSamplerParameteri(id, GL_TEXTURE_WRAP_R, static_cast<GLint>(gl_wrap(s.address_w)));
    glSamplerParameterf(id, GL_TEXTURE_MIN_LOD, std::max(s.min_lod, -1000.f));
    glSamplerParameterf(id, GL_TEXTURE_MAX_LOD, std::min(s.max_lod, 1000.f));
    glSamplerParameterf(id, GL_TEXTURE_LOD_BIAS, s.lod_bias);
    if (aniso)
        glSamplerParameterf(id, GL_TEXTURE_MAX_ANISOTROPY, 16.f);
    return id;
}

GLuint compile_shader(GLenum stage, const std::string& source, const std::string& name,
                      std::string& error) {
    const GLuint shader = glCreateShader(stage);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
        glGetShaderInfoLog(shader, length, nullptr, log.data());
        error += name + ": " + log.c_str() + "\n";
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// Every folder of a shader pack holding effects or headers: ReShade looks
// for includes in all of them.
std::vector<fs::path> include_dirs(const fs::path& root) {
    std::set<fs::path> dirs;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::follow_directory_symlink, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto ext = it->path().extension();
        if (ext == ".fxh" || ext == ".fx")
            dirs.insert(it->path().parent_path());
    }
    return {dirs.begin(), dirs.end()};
}

// The root a file lives under, or empty.
fs::path root_of(const fs::path& file) {
    for (const auto& root : ReshadeEffect::roots()) {
        const auto rel = file.lexically_relative(root);
        if (!rel.empty() && *rel.begin() != "..")
            return root;
    }
    return {};
}

// A texture file named by an effect: next to it, or anywhere in its pack.
fs::path find_texture(const fs::path& effect, const std::string& name) {
    std::error_code ec;
    const fs::path direct = effect.parent_path() / name;
    if (fs::is_regular_file(direct, ec))
        return direct;
    const fs::path root = root_of(effect);
    if (root.empty())
        return {};
    const fs::path wanted = fs::path(name).filename();
    std::string lower = wanted.string();
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    fs::path fallback;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::follow_directory_symlink, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        const fs::path f = it->path().filename();
        if (f == wanted)
            return it->path();
        // Packs made on Windows do not always match the case of the name.
        std::string l = f.string();
        std::transform(l.begin(), l.end(), l.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (fallback.empty() && l == lower)
            fallback = it->path();
    }
    return fallback;
}

std::string strip_comments(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s.compare(i, 2, "//") == 0) {
            i = s.find('\n', i);
            if (i == std::string::npos) break;
            out += '\n';
        } else if (s[i] == '"') {             // string literals: tooltips mention "technique"
            i = s.find('"', i + 1);
            if (i == std::string::npos) break;
            out += "\"\"";
        } else if (s.compare(i, 2, "/*") == 0) {
            i = s.find("*/", i + 2);
            if (i == std::string::npos) break;
            ++i;
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace

// ── Library helpers ───────────────────────────────────────────────────────

std::vector<fs::path> ReshadeEffect::roots() {
    std::vector<fs::path> candidates{PostLibrary::user_dir() / "reshade"};
    if (const char* home = std::getenv("HOME"))
        candidates.push_back(fs::path(home) / ".local/share/reshade");
    candidates.push_back("/usr/share/reshade");
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& c : candidates)
        if (fs::is_directory(c, ec))
            out.push_back(c);
    return out;
}

std::vector<std::string> ReshadeEffect::techniques(const fs::path& file) {
    std::ifstream in(file);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = strip_comments(ss.str());
    std::vector<std::string> out;
    const auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    for (std::size_t at = text.find("technique"); at != std::string::npos;
         at = text.find("technique", at + 9)) {
        if ((at > 0 && ident(text[at - 1])) || at + 9 >= text.size() ||
            !std::isspace(static_cast<unsigned char>(text[at + 9])))
            continue;
        std::size_t b = at + 9;
        while (b < text.size() && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
        std::size_t e = b;
        while (e < text.size() && ident(text[e])) ++e;
        // A declaration: "technique Name <annotations> {" or "technique Name {".
        std::size_t after = e;
        while (after < text.size() && std::isspace(static_cast<unsigned char>(text[after]))) ++after;
        if (e > b && !std::isdigit(static_cast<unsigned char>(text[b])) && after < text.size() &&
            (text[after] == '<' || text[after] == '{'))
            out.push_back(text.substr(b, e - b));
    }
    std::vector<std::string> unique;
    for (auto& t : out)
        if (std::find(unique.begin(), unique.end(), t) == unique.end())
            unique.push_back(std::move(t));
    return unique;
}

std::vector<PostOption> ReshadeEffect::read_parameters(const fs::path& file, const std::string& technique) {
    ReshadeEffect effect;
    std::string error;
    effect.load(file, technique, error);
    return effect.parameters();
}

// ── Compile (no GL) ───────────────────────────────────────────────────────

ReshadeEffect::ReshadeEffect() = default;

ReshadeEffect::~ReshadeEffect() = default;

bool ReshadeEffect::load(const fs::path& file, const std::string& technique, std::string& error) {
    m_file      = file;
    m_technique = technique;
    m_start = m_last = std::chrono::steady_clock::now();
    // Parameters do not depend on the size; any size will do until the
    // first frame says which one is needed.
    return compile(1920, 1080, error);
}

bool ReshadeEffect::compile(int width, int height, std::string& error) {
    m_module.reset();
    m_code.clear();
    m_uniforms.clear();

    preprocessor pp;
    pp.add_macro_definition("__RESHADE__", "60800");
    pp.add_macro_definition("__RESHADE_PERMUTATION__", "0");
    pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "0");
    pp.add_macro_definition("__VENDOR__", "0");
    pp.add_macro_definition("__DEVICE__", "0");
    pp.add_macro_definition("__RENDERER__", "0x14500");   // OpenGL 4.5
    pp.add_macro_definition("__APPLICATION__", "0");
    pp.add_macro_definition("BUFFER_WIDTH", std::to_string(width));
    pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(height));
    pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
    pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
    pp.add_macro_definition("BUFFER_COLOR_SPACE", "1");          // sRGB
    pp.add_macro_definition("BUFFER_COLOR_FORMAT", "28");        // RGBA8 unorm
    pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "8");
    pp.add_include_path(m_file.parent_path());
    for (const auto& root : roots())
        for (const auto& dir : include_dirs(root))
            pp.add_include_path(dir);
    // The aliases ReShade itself adds for effects written for older versions.
    pp.append_string(
        "#define tex2Doffset(s, coords, offset) tex2D(s, coords, offset)\n"
        "#define tex2Dlodoffset(s, coords, offset) tex2Dlod(s, coords, offset)\n"
        "#define tex2Dgather(s, t, c) tex2Dgather##c(s, t)\n"
        "#define tex2Dgatheroffset(s, t, o, c) tex2Dgather##c(s, t, o)\n"
        "#define tex2Dgather0 tex2DgatherR\n"
        "#define tex2Dgather1 tex2DgatherG\n"
        "#define tex2Dgather2 tex2DgatherB\n"
        "#define tex2Dgather3 tex2DgatherA\n");
    if (!pp.append_file(m_file)) {
        error = pp.errors().empty() ? "cannot read " + m_file.string() : pp.errors();
        return false;
    }

    std::unique_ptr<codegen> backend(
        create_codegen_glsl(false, false, false, false, /*flip_vert_y=*/true));
    parser parse;
    if (!parse.parse(pp.output(), backend.get())) {
        error = parse.errors();
        return false;
    }
    m_module = std::make_unique<effect_module>(backend->module());

    const technique* tech = nullptr;
    for (const auto& t : m_module->techniques)
        if (m_technique.empty() || t.name == m_technique) {
            tech = &t;
            break;
        }
    if (!tech) {
        error = m_technique.empty() ? "the effect has no technique"
                                    : "the effect has no technique named " + m_technique;
        m_module.reset();
        return false;
    }
    for (const auto& pass : tech->passes)
        for (const std::string* entry : {&pass.vs_entry_point, &pass.ps_entry_point, &pass.cs_entry_point}) {
            if (entry->empty() || m_code.count(*entry))
                continue;
            std::string code, unused, errors;
            if (!backend->assemble_code_for_entry_point(*entry, code, unused, errors)) {
                error = "cannot generate " + *entry + ": " + errors;
                m_module.reset();
                return false;
            }
            m_code[*entry] = std::move(code);
        }

    // Uniforms: sources the host fills, and parameters the player sets.
    std::vector<PostOption> params;
    for (const auto& u : m_module->uniforms) {
        Uniform entry;
        entry.info   = u;
        entry.source = annotation_string(u.annotations, "source");
        if (!entry.source.empty()) {
            if (const annotation* a = find_annotation(u.annotations, "min")) entry.ann_min = annotation_float(*a);
            if (const annotation* a = find_annotation(u.annotations, "max")) entry.ann_max = annotation_float(*a);
            if (const annotation* a = find_annotation(u.annotations, "step")) {
                entry.step[0] = annotation_float(*a, 0);
                entry.step[1] = annotation_float(*a, 1);
            }
            if (const annotation* a = find_annotation(u.annotations, "smoothing"))
                entry.smoothing = annotation_float(*a);
            if (entry.source == "random" && !find_annotation(u.annotations, "max"))
                entry.ann_max = 32767.f;
            m_uniforms.push_back(std::move(entry));
            continue;
        }
        const annotation* hidden = find_annotation(u.annotations, "hidden");
        const bool shown = !(hidden && annotation_float(*hidden) != 0.f) && u.type.is_numeric() &&
                           !u.type.is_matrix() && !u.type.is_array();
        if (shown) {
            const std::string ui_type = annotation_string(u.annotations, "ui_type");
            std::string label = annotation_string(u.annotations, "ui_label");
            if (label.empty())
                label = u.name;
            // Combo and radio lists: names separated by NULs.
            std::vector<std::string> items;
            const std::string list = annotation_string(u.annotations, "ui_items");
            for (std::size_t b = 0; b < list.size();) {
                const std::size_t e = std::min(list.find('\0', b), list.size());
                if (e > b) items.push_back(list.substr(b, e - b));
                b = e + 1;
            }
            const annotation* amin  = find_annotation(u.annotations, "ui_min");
            const annotation* amax  = find_annotation(u.annotations, "ui_max");
            const annotation* astep = find_annotation(u.annotations, "ui_step");
            const unsigned n = u.type.components();
            static const char* const kXyzw[] = {"X", "Y", "Z", "W"};
            static const char* const kRgba[] = {"Red", "Green", "Blue", "Alpha"};
            for (unsigned i = 0; i < n; ++i) {
                PostOption o;
                o.name  = n == 1 ? u.name : u.name + "[" + std::to_string(i) + "]";
                o.label = n == 1 ? label
                                 : label + " (" + (ui_type == "color" ? kRgba[i] : kXyzw[i]) + ")";
                o.def   = initializer_component(u, i);
                if (u.type.is_boolean()) {
                    o.type = PostOption::Type::Bool;
                    o.min = 0.f; o.max = 1.f; o.step = 1.f;
                } else {
                    o.type = u.type.is_floating_point() ? PostOption::Type::Float : PostOption::Type::Int;
                    if (!items.empty()) {
                        o.items = items;
                        o.min = 0.f;
                        o.max = static_cast<float>(items.size() - 1);
                    } else {
                        o.min = amin ? annotation_float(*amin, i)
                                     : ui_type == "color" ? 0.f : std::min(0.f, o.def);
                        o.max = amax ? annotation_float(*amax, i)
                                     : ui_type == "color" ? 1.f
                                     : std::max({1.f, o.def * 2.f, o.min + 1.f});
                    }
                    o.step = astep ? annotation_float(*astep, i)
                           : o.type == PostOption::Type::Int ? 1.f : (o.max - o.min) / 100.f;
                    if (o.step <= 0.f)
                        o.step = o.type == PostOption::Type::Int ? 1.f : 0.01f;
                }
                entry.options.push_back(o.name);
                entry.defaults.push_back(o.def);
                params.push_back(std::move(o));
            }
        }
        m_uniforms.push_back(std::move(entry));
    }
    m_parameters = std::move(params);
    m_ubo_data.assign((m_module->total_uniform_size + 15) & ~15u, 0);
    for (const auto& u : m_uniforms)
        if (u.source.empty())
            put_initializer(m_ubo_data, u.info);
    m_compiled_w = width;
    m_compiled_h = height;
    return true;
}

// ── GL objects ────────────────────────────────────────────────────────────

ReshadeEffect::Shared::~Shared() {
    for (auto& [name, t] : textures) {
        if (t.srgb_view) glDeleteTextures(1, &t.srgb_view);
        if (t.id) glDeleteTextures(1, &t.id);
    }
}

namespace {
// Live shared texture sets, by "<file>|<width>x<height>".
std::map<std::string, std::weak_ptr<void>>& shared_registry() {
    static std::map<std::string, std::weak_ptr<void>> registry;
    return registry;
}
} // namespace

bool ReshadeEffect::build(std::string& error) {
    destroy_gl();
    const technique* tech = nullptr;
    for (const auto& t : m_module->techniques)
        if (m_technique.empty() || t.name == m_technique) {
            tech = &t;
            break;
        }
    if (!tech) {
        error = "technique missing";
        return false;
    }
    const int w = m_compiled_w, h = m_compiled_h;

    // Two copies of the image: passes read one and draw into the other.
    for (int i = 0; i < 2; ++i) {
        glCreateTextures(GL_TEXTURE_2D, 1, &m_back[i]);
        glTextureStorage2D(m_back[i], 1, GL_RGBA8, w, h);
        glGenTextures(1, &m_back_srgb[i]);
        glTextureView(m_back_srgb[i], GL_TEXTURE_2D, m_back[i], GL_SRGB8_ALPHA8, 0, 1, 0, 1);
    }
    m_cur = 0;
    {
        glCreateTextures(GL_TEXTURE_2D, 1, &m_blank_depth);
        glTextureStorage2D(m_blank_depth, 1, GL_R32F, 1, 1);
        const float zero = 0.f;
        glTextureSubImage2D(m_blank_depth, 0, 0, 0, 1, 1, GL_RED, GL_FLOAT, &zero);
    }

    {
        const std::string key = m_file.string() + "|" + std::to_string(w) + "x" + std::to_string(h);
        auto& slot = shared_registry()[key];
        m_shared = std::static_pointer_cast<Shared>(slot.lock());
        if (!m_shared) {
            m_shared = std::make_shared<Shared>();
            slot = m_shared;
        }
    }
    for (const auto& t : m_module->textures) {
        if (t.semantic.empty())
            if (const auto it = m_shared->textures.find(t.unique_name); it != m_shared->textures.end()) {
                m_textures.push_back(it->second);       // made by another stage of this file
                continue;
            }
        Texture tex;
        tex.unique_name   = t.unique_name;
        tex.semantic      = t.semantic;
        tex.render_target = t.render_target;
        if (!t.semantic.empty()) {          // COLOR, DEPTH, ...: provided, not created
            m_textures.push_back(std::move(tex));
            continue;
        }
        tex.target = t.type == texture_type::texture_1d ? GL_TEXTURE_1D
                   : t.type == texture_type::texture_3d ? GL_TEXTURE_3D : GL_TEXTURE_2D;
        tex.format = gl_format(t.format);
        tex.w = static_cast<int>(std::max(1u, t.width));
        tex.h = static_cast<int>(std::max(1u, t.height));
        tex.d = std::max<int>(1, t.depth);
        tex.levels = std::max<int>(1, t.levels);
        glCreateTextures(tex.target, 1, &tex.id);
        if (tex.target == GL_TEXTURE_1D)
            glTextureStorage1D(tex.id, tex.levels, tex.format, tex.w);
        else if (tex.target == GL_TEXTURE_3D)
            glTextureStorage3D(tex.id, tex.levels, tex.format, tex.w, tex.h, tex.d);
        else
            glTextureStorage2D(tex.id, tex.levels, tex.format, tex.w, tex.h);
        for (int level = 0; level < tex.levels; ++level)
            glClearTexImage(tex.id, level, GL_RGBA, GL_FLOAT, nullptr);

        // Images the effect ships with (lookup tables, masks, noise).
        const std::string source = annotation_string(t.annotations, "source");
        if (!source.empty() && tex.target == GL_TEXTURE_2D) {
            const fs::path file = find_texture(m_file, source);
            try {
                if (file.empty())
                    throw std::runtime_error("not found");
                auto pixbuf = image::load_ref(file);
                if (!pixbuf)
                    throw std::runtime_error("cannot read " + file.string());
                if (pixbuf->get_width() != tex.w || pixbuf->get_height() != tex.h)
                    pixbuf = pixbuf->scale_simple(tex.w, tex.h, Gdk::InterpType::BILINEAR);
                if (!pixbuf->get_has_alpha())
                    pixbuf = pixbuf->add_alpha(false, 0, 0, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, pixbuf->get_rowstride() / 4);
                glTextureSubImage2D(tex.id, 0, 0, 0, tex.w, tex.h, GL_RGBA, GL_UNSIGNED_BYTE,
                                    pixbuf->get_pixels());
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                if (tex.levels > 1)
                    glGenerateTextureMipmap(tex.id);
            } catch (const std::exception& e) {   // also Glib::Error
                error += t.name + ": texture " + source + ": " + e.what() + "\n";
            }
        }
        if (tex.format == GL_RGBA8 && tex.target == GL_TEXTURE_2D) {
            glGenTextures(1, &tex.srgb_view);
            glTextureView(tex.srgb_view, GL_TEXTURE_2D, tex.id, GL_SRGB8_ALPHA8, 0,
                          static_cast<GLuint>(tex.levels), 0, 1);
        }
        m_shared->textures[tex.unique_name] = tex;
        m_textures.push_back(std::move(tex));
    }

    for (const auto& s : m_module->samplers)
        m_samplers.push_back(make_sampler(s));

    // Programs, one per pass; each entry point is compiled once.
    std::map<std::string, GLuint> shaders;
    std::string compile_errors;
    const auto shader = [&](const std::string& entry, GLenum stage) -> GLuint {
        if (const auto it = shaders.find(entry); it != shaders.end())
            return it->second;
        const GLuint id = compile_shader(stage, m_code[entry], entry, compile_errors);
        shaders[entry] = id;
        return id;
    };
    for (const auto& info : tech->passes) {
        auto pass = std::make_unique<Pass>();
        pass->info = info;
        pass->compute = !info.cs_entry_point.empty();
        std::vector<GLuint> stages;
        if (pass->compute) {
            stages.push_back(shader(info.cs_entry_point, GL_COMPUTE_SHADER));
        } else {
            stages.push_back(shader(info.vs_entry_point, GL_VERTEX_SHADER));
            if (!info.ps_entry_point.empty())
                stages.push_back(shader(info.ps_entry_point, GL_FRAGMENT_SHADER));
        }
        if (std::find(stages.begin(), stages.end(), 0u) == stages.end()) {
            pass->program = glCreateProgram();
            for (GLuint s : stages)
                glAttachShader(pass->program, s);
            glLinkProgram(pass->program);
            GLint ok = 0;
            glGetProgramiv(pass->program, GL_LINK_STATUS, &ok);
            if (!ok) {
                char log[2048] = {};
                glGetProgramInfoLog(pass->program, sizeof(log), nullptr, log);
                compile_errors += "pass " + (info.name.empty() ? std::to_string(m_passes.size()) : info.name) +
                                  ": " + log + "\n";
                glDeleteProgram(pass->program);
                pass->program = 0;
            }
        }
        m_passes.push_back(std::move(pass));
    }
    for (const auto& [name, id] : shaders)
        if (id) glDeleteShader(id);
    if (!compile_errors.empty()) {
        error += compile_errors;
        destroy_gl();
        return false;
    }

    glCreateBuffers(1, &m_ubo);
    glNamedBufferData(m_ubo, static_cast<GLsizeiptr>(std::max<std::size_t>(m_ubo_data.size(), 16)),
                      nullptr, GL_DYNAMIC_DRAW);
    glCreateFramebuffers(1, &m_fbo);
    glCreateFramebuffers(1, &m_read_fbo);
    glCreateVertexArrays(1, &m_vao);
    m_built = true;
    return true;
}

void ReshadeEffect::destroy_gl() {
    m_textures.clear();          // owned by m_shared
    m_shared.reset();
    for (GLuint s : m_samplers)
        glDeleteSamplers(1, &s);
    m_samplers.clear();
    for (auto& p : m_passes)
        if (p->program) glDeleteProgram(p->program);
    m_passes.clear();
    for (int i = 0; i < 2; ++i) {
        if (m_back_srgb[i]) glDeleteTextures(1, &m_back_srgb[i]);
        if (m_back[i]) glDeleteTextures(1, &m_back[i]);
        m_back[i] = m_back_srgb[i] = 0;
    }
    if (m_blank_depth) glDeleteTextures(1, &m_blank_depth);
    if (m_stencil) glDeleteRenderbuffers(1, &m_stencil);
    if (m_ubo) glDeleteBuffers(1, &m_ubo);
    if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
    if (m_read_fbo) glDeleteFramebuffers(1, &m_read_fbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    m_blank_depth = m_stencil = m_ubo = m_fbo = m_read_fbo = m_vao = 0;
    m_stencil_w = m_stencil_h = 0;
    m_built = false;
}

void ReshadeEffect::release() {
    destroy_gl();
    m_failed = false;
}

GLuint ReshadeEffect::stencil_for(int w, int h) {
    if (m_stencil && m_stencil_w == w && m_stencil_h == h)
        return m_stencil;
    if (m_stencil) glDeleteRenderbuffers(1, &m_stencil);
    glCreateRenderbuffers(1, &m_stencil);
    glNamedRenderbufferStorage(m_stencil, GL_DEPTH24_STENCIL8, w, h);
    m_stencil_w = w;
    m_stencil_h = h;
    return m_stencil;
}

GLuint ReshadeEffect::texture_for(const std::string& unique_name, bool srgb) {
    for (const auto& t : m_textures) {
        if (t.unique_name != unique_name)
            continue;
        if (t.semantic == "COLOR")
            return srgb ? m_back_srgb[m_cur] : m_back[m_cur];
        if (!t.semantic.empty())
            return m_blank_depth;           // DEPTH: the game gives none
        return srgb && t.srgb_view ? t.srgb_view : t.id;
    }
    return 0;
}

// ── Frame ─────────────────────────────────────────────────────────────────

void ReshadeEffect::update_uniforms(float frame_ms) {
    const float now_ms = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - m_start).count();
    for (auto& u : m_uniforms) {
        const unsigned n = u.info.type.components();
        if (!u.options.empty()) {
            std::vector<float> v(u.options.size());
            for (std::size_t i = 0; i < v.size(); ++i) {
                const auto it = m_values.find(u.options[i]);
                v[i] = it != m_values.end() ? it->second : u.defaults[i];
            }
            put_floats(m_ubo_data, u.info, v.data(), static_cast<unsigned>(v.size()));
            continue;
        }
        if (u.source.empty())
            continue;
        float v[16] = {};
        if (u.source == "timer") {
            v[0] = now_ms;
        } else if (u.source == "frametime") {
            v[0] = frame_ms;
        } else if (u.source == "framecount") {
            v[0] = static_cast<float>(m_frame_count);
        } else if (u.source == "random") {
            const int lo = static_cast<int>(u.ann_min), hi = static_cast<int>(u.ann_max);
            for (unsigned i = 0; i < n; ++i)
                v[i] = static_cast<float>(lo + (hi > lo ? std::rand() % (hi - lo + 1) : 0));
        } else if (u.source == "pingpong") {
            const float step = u.step[1] == 0.f ? u.step[0]
                             : u.step[0] + static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX) * (u.step[1] - u.step[0]);
            u.pp_value += step * u.pp_dir * frame_ms / 1000.f;
            if (u.pp_value >= u.ann_max) { u.pp_value = u.ann_max - u.smoothing; u.pp_dir = -1.f; }
            else if (u.pp_value <= u.ann_min) { u.pp_value = u.ann_min + u.smoothing; u.pp_dir = 1.f; }
            v[0] = u.pp_value;
            v[1] = u.pp_dir;
        } else if (u.source == "date") {
            const std::time_t t = std::time(nullptr);
            std::tm local{};
            localtime_r(&t, &local);
            v[0] = static_cast<float>(local.tm_year + 1900);
            v[1] = static_cast<float>(local.tm_mon + 1);
            v[2] = static_cast<float>(local.tm_mday);
            v[3] = static_cast<float>(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
        }
        // key, mouse*, overlay_*, screenshot, bufready_depth: nothing to report.
        put_floats(m_ubo_data, u.info, v, n);
    }
    if (!m_ubo_data.empty())
        glNamedBufferSubData(m_ubo, 0, static_cast<GLsizeiptr>(m_ubo_data.size()), m_ubo_data.data());
}

void ReshadeEffect::run_pass(const Pass& pass) {
    const auto& info = pass.info;
    for (const auto& b : info.texture_bindings) {
        const auto& s = m_module->samplers[b.index];
        glBindTextureUnit(b.entry_point_binding, texture_for(s.texture_name, b.srgb));
        glBindSampler(b.entry_point_binding, m_samplers[b.index]);
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_ubo);
    glUseProgram(pass.program);

    const auto find = [this](const std::string& name) -> Texture* {
        for (auto& t : m_textures)
            if (t.unique_name == name) return &t;
        return nullptr;
    };

    if (pass.compute) {
        for (const auto& b : info.storage_bindings) {
            const auto& st = m_module->storages[b.index];
            if (Texture* t = find(st.texture_name); t && t->id)
                glBindImageTexture(b.entry_point_binding, t->id, st.level,
                                   t->target == GL_TEXTURE_3D ? GL_TRUE : GL_FALSE, 0,
                                   GL_READ_WRITE, t->format);
        }
        glDispatchCompute(std::max(1u, info.viewport_width), std::max(1u, info.viewport_height),
                          std::max(1u, info.viewport_dispatch_z));
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        if (info.generate_mipmaps)
            for (const auto& b : info.storage_bindings)
                if (Texture* t = find(m_module->storages[b.index].texture_name); t && t->levels > 1)
                    glGenerateTextureMipmap(t->id);
        for (const auto& b : info.storage_bindings)
            glBindImageTexture(b.entry_point_binding, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
        return;
    }

    const bool to_back = info.render_target_names[0].empty();
    int target_w = m_compiled_w, target_h = m_compiled_h;
    int count = 0;
    const int other = 1 - m_cur;
    if (to_back) {
        // Passes that do not cover every pixel keep what is already there.
        const bool partial = info.blend_enable[0] || info.stencil_enable ||
                             info.num_vertices != 3 ||
                             info.topology != primitive_topology::triangle_list;
        if (partial && !info.clear_render_targets)
            glCopyImageSubData(m_back[m_cur], GL_TEXTURE_2D, 0, 0, 0, 0, m_back[other], GL_TEXTURE_2D,
                               0, 0, 0, 0, m_compiled_w, m_compiled_h, 1);
        glNamedFramebufferTexture(m_fbo, GL_COLOR_ATTACHMENT0,
                                  info.srgb_write_enable ? m_back_srgb[other] : m_back[other], 0);
        count = 1;
    } else {
        for (int k = 0; k < 8 && !info.render_target_names[k].empty(); ++k) {
            Texture* t = find(info.render_target_names[k]);
            if (!t)
                break;
            glNamedFramebufferTexture(m_fbo, GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(k),
                                      info.srgb_write_enable && t->srgb_view ? t->srgb_view : t->id, 0);
            if (k == 0) {
                target_w = t->w;
                target_h = t->h;
            }
            count = k + 1;
        }
    }
    for (int k = count; k < 8; ++k)
        glNamedFramebufferTexture(m_fbo, GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(k), 0, 0);
    GLenum buffers[8];
    for (int k = 0; k < count; ++k)
        buffers[k] = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(k);
    glNamedFramebufferDrawBuffers(m_fbo, count, buffers);

    // The stencil buffer exists at the image's size only.
    const bool stencil = info.stencil_enable && target_w == m_compiled_w && target_h == m_compiled_h;
    glNamedFramebufferRenderbuffer(m_fbo, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                   stencil ? stencil_for(target_w, target_h) : 0);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    if (stencil && !m_stencil_cleared) {
        glClearNamedFramebufferfi(m_fbo, GL_DEPTH_STENCIL, 0, 1.f, 0);
        m_stencil_cleared = true;
    }

    glViewport(0, 0, info.viewport_width ? static_cast<GLsizei>(info.viewport_width) : target_w,
               info.viewport_height ? static_cast<GLsizei>(info.viewport_height) : target_h);
    if (info.srgb_write_enable) glEnable(GL_FRAMEBUFFER_SRGB);
    else                        glDisable(GL_FRAMEBUFFER_SRGB);
    if (info.clear_render_targets) {
        const float zero[4] = {0.f, 0.f, 0.f, 0.f};
        for (int k = 0; k < count; ++k)
            glClearNamedFramebufferfv(m_fbo, GL_COLOR, k, zero);
    }
    for (int k = 0; k < count; ++k) {
        const auto i = static_cast<GLuint>(k);
        if (info.blend_enable[k]) {
            glEnablei(GL_BLEND, i);
            glBlendFuncSeparatei(i, gl_blend_factor(info.source_color_blend_factor[k]),
                                 gl_blend_factor(info.dest_color_blend_factor[k]),
                                 gl_blend_factor(info.source_alpha_blend_factor[k]),
                                 gl_blend_factor(info.dest_alpha_blend_factor[k]));
            glBlendEquationSeparatei(i, gl_blend_op(info.color_blend_op[k]),
                                     gl_blend_op(info.alpha_blend_op[k]));
        } else {
            glDisablei(GL_BLEND, i);
        }
        const std::uint8_t m = info.render_target_write_mask[k];
        glColorMaski(i, (m & 1) != 0, (m & 2) != 0, (m & 4) != 0, (m & 8) != 0);
    }
    if (stencil) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(gl_stencil_func(info.stencil_comparison_func), info.stencil_reference_value,
                      info.stencil_read_mask);
        glStencilOp(gl_stencil_op(info.stencil_fail_op), gl_stencil_op(info.stencil_depth_fail_op),
                    gl_stencil_op(info.stencil_pass_op));
        glStencilMask(info.stencil_write_mask);
    }

    glBindVertexArray(m_vao);
    glDrawArrays(gl_topology(info.topology), 0, static_cast<GLsizei>(info.num_vertices));

    for (int k = 0; k < count; ++k) {
        glDisablei(GL_BLEND, static_cast<GLuint>(k));
        glColorMaski(static_cast<GLuint>(k), GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    if (stencil) {
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
    }
    glDisable(GL_FRAMEBUFFER_SRGB);

    if (to_back) {
        m_cur = other;
    } else if (info.generate_mipmaps) {
        for (int k = 0; k < count; ++k)
            if (Texture* t = find(info.render_target_names[k]); t && t->levels > 1)
                glGenerateTextureMipmap(t->id);
    }
}

bool ReshadeEffect::render(const Frame& frame) {
    const auto passthrough = [&] {
        GLuint read = 0;
        glCreateFramebuffers(1, &read);
        glNamedFramebufferTexture(read, GL_COLOR_ATTACHMENT0, frame.input, 0);
        frame.bind_output();
        GLint draw = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        glBlitNamedFramebuffer(read, static_cast<GLuint>(draw), 0, 0, frame.input_w, frame.input_h,
                               frame.x, frame.y, frame.x + frame.w, frame.y + frame.h,
                               GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glDeleteFramebuffers(1, &read);
    };
    if (frame.w <= 0 || frame.h <= 0 || !frame.input)
        return false;

    const auto now = std::chrono::steady_clock::now();
    const float frame_ms = std::chrono::duration<float, std::milli>(now - m_last).count();
    m_last = now;
    m_frame_count = frame.frame_count;

    if (frame.w != m_compiled_w || frame.h != m_compiled_h || !m_module) {
        // Built for another size: wait until the new one settles.
        if (frame.w != m_pending_w || frame.h != m_pending_h) {
            m_pending_w = frame.w;
            m_pending_h = frame.h;
            m_pending_frames = 0;
        }
        if (m_built && ++m_pending_frames < 8) {
            passthrough();
            return true;
        }
        destroy_gl();
        m_failed = false;
        std::string error;
        if (!compile(frame.w, frame.h, error)) {
            m_failed = true;
            m_error  = error;
        }
    }
    if (!m_failed && !m_built) {
        std::string error;
        if (!build(error)) {
            m_failed = true;
            m_error  = error;
        } else if (!error.empty()) {
            m_error = error;                // missing textures: runs, but say so
        }
    }
    if (m_failed) {
        passthrough();
        return false;
    }

    // The image in, top row first, at the size the effect was built for.
    glNamedFramebufferTexture(m_read_fbo, GL_COLOR_ATTACHMENT0, 0, 0);
    GLuint input_fbo = 0;
    glCreateFramebuffers(1, &input_fbo);
    glNamedFramebufferTexture(input_fbo, GL_COLOR_ATTACHMENT0, frame.input, 0);
    glNamedFramebufferTexture(m_fbo, GL_COLOR_ATTACHMENT0, m_back[m_cur], 0);
    for (int k = 1; k < 8; ++k)
        glNamedFramebufferTexture(m_fbo, GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(k), 0, 0);
    glNamedFramebufferRenderbuffer(m_fbo, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
    glNamedFramebufferDrawBuffer(m_fbo, GL_COLOR_ATTACHMENT0);
    glBlitNamedFramebuffer(input_fbo, m_fbo, 0, 0, frame.input_w, frame.input_h,
                           0, m_compiled_h, m_compiled_w, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glDeleteFramebuffers(1, &input_fbo);

    update_uniforms(frame_ms);
    m_stencil_cleared = false;
    GLuint max_unit = 0, max_image = 0;
    for (const auto& pass : m_passes) {
        run_pass(*pass);
        for (const auto& b : pass->info.texture_bindings)
            max_unit = std::max(max_unit, b.entry_point_binding + 1);
        for (const auto& b : pass->info.storage_bindings)
            max_image = std::max(max_image, b.entry_point_binding + 1);
    }

    // Leave the GL state as the rest of the window expects it.
    for (GLuint u = 0; u < max_unit; ++u) {
        glBindTextureUnit(u, 0);
        glBindSampler(u, 0);
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    for (GLuint u = 0; u < max_image; ++u)
        glBindImageTexture(u, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);

    // The result out, back to the bottom row first.
    glNamedFramebufferTexture(m_read_fbo, GL_COLOR_ATTACHMENT0, m_back[m_cur], 0);
    frame.bind_output();
    GLint draw = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glBlitNamedFramebuffer(m_read_fbo, static_cast<GLuint>(draw), 0, 0, m_compiled_w, m_compiled_h,
                           frame.x, frame.y + frame.h, frame.x + frame.w, frame.y,
                           GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glViewport(frame.x, frame.y, frame.w, frame.h);
    return true;
}

} // namespace ds::frontend
