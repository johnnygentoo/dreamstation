// SPDX-License-Identifier: GPL-3.0-or-later
#include "post_chain.hpp"
#include "../config.hpp"
#include <dreamstation/common/log.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ds::frontend {

namespace fs = std::filesystem;

const std::map<std::string, std::string>& builtin_post_shaders();

namespace {

// Fullscreen triangle. v_tex0 has its origin at the top-left, as the shader
// dialect expects; the prelude flips lookups back to GL's bottom-left.
const char* const kVertex = R"glsl(#version 330 core
out vec2 v_tex0;
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    v_tex0 = vec2(uv.x, 1.0 - uv.y);
}
)glsl";

// Everything a shader in the Dolphin/DuckStation dialect may rely on.
const char* const kPrelude = R"glsl(#version 330 core
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define uint2 uvec2
#define uint3 uvec3
#define uint4 uvec4
#define float2x2 mat2
#define float3x3 mat3
#define float4x4 mat4
#define frac fract
#define lerp mix
#define saturate(x) clamp((x), 0.0, 1.0)
#define mul(a, b) ((a) * (b))
#define CONSTANT const
#define BEGIN_ARRAY(type, size) type[size](
#define END_ARRAY )
#define GetOption(x) (x)
#define OptionEnabled(x) ((x) != 0)

uniform sampler2D samp0;
uniform vec2  u_window_size;      // this stage's output size
uniform vec2  u_internal_size;    // the game's rendering resolution
uniform vec2  u_native_size;      // the console's own resolution
uniform vec2  u_origin;           // output rectangle origin in the framebuffer
uniform float u_fb_height;
uniform float u_time;

in  vec2 v_tex0;
out vec4 o_col0;

vec2 ds_flip(vec2 uv) { return vec2(uv.x, 1.0 - uv.y); }
vec4  Sample() { return texture(samp0, ds_flip(v_tex0)); }
vec4  SampleLocation(vec2 location) { return texture(samp0, ds_flip(location)); }
#define SampleOffset(offset) \
    texture(samp0, ds_flip(v_tex0 + vec2(offset) / vec2(textureSize(samp0, 0))))
vec2  GetFragCoord() {
    return vec2(gl_FragCoord.x - u_origin.x, u_fb_height - gl_FragCoord.y - u_origin.y);
}
vec2  GetCoordinates() { return v_tex0; }
vec2  GetWindowSize() { return u_window_size; }
vec2  GetInvWindowSize() { return 1.0 / u_window_size; }
vec2  GetViewportSize() { return u_window_size; }
vec2  GetWindowToViewportRatio() { return vec2(1.0, 1.0); }
vec2  GetInternalSize() { return u_internal_size; }
vec2  GetInternalPixelSize() { return 1.0 / u_internal_size; }
vec2  GetInvInternalPixelSize() { return 1.0 / u_internal_size; }
vec2  GetNativeSize() { return u_native_size; }
vec2  GetNativePixelSize() { return 1.0 / u_native_size; }
vec2  GetInvNativePixelSize() { return 1.0 / u_native_size; }
float GetUpscaleMultiplier() { return u_internal_size.y / u_native_size.y; }
float GetTime() { return u_time; }
void  SetOutput(vec4 color) { o_col0 = color; }
vec2  GetResolution() { return u_window_size; }
vec2  GetInvResolution() { return 1.0 / u_window_size; }
vec2  GetOriginalSize() { return u_internal_size; }
vec2  GetPaddedOriginalSize() { return u_internal_size; }
vec2  GetWindowResolution() { return u_window_size; }
)glsl";

// Box-filter the input down to this stage's output size.
const char* const kDownsample = R"glsl(
void main() {
    vec2 ratio = u_internal_size / u_window_size;
    ivec2 taps = clamp(ivec2(ceil(ratio)), ivec2(1), ivec2(8));
    vec3 sum = vec3(0.0);
    for (int y = 0; y < taps.y; ++y)
        for (int x = 0; x < taps.x; ++x) {
            vec2 off = (vec2(x, y) + 0.5) / vec2(taps) - 0.5;
            sum += SampleLocation(GetCoordinates() + off / u_window_size).rgb;
        }
    SetOutput(vec4(sum / float(taps.x * taps.y), 1.0));
}
)glsl";

const char* const kPassthrough = R"glsl(
void main() { SetOutput(vec4(Sample().rgb, 1.0)); }
)glsl";

std::string trim(std::string s) {
    const auto ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && ws(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && ws(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

// Parse the `[configuration]` comment block.
std::vector<PostOption> parse_options(const std::string& source) {
    std::vector<PostOption> out;
    const auto begin = source.find("[configuration]");
    if (begin == std::string::npos)
        return out;
    const auto end = source.find("*/", begin);
    std::istringstream lines(source.substr(begin, end == std::string::npos ? end : end - begin));

    PostOption cur;
    bool have = false, vector_option = false;
    const auto flush = [&] {
        // Vector options have no UI yet; they keep the value 0 in the shader.
        if (have && !cur.name.empty() && !vector_option)
            out.push_back(cur);
        cur = {};
        have = vector_option = false;
    };
    for (std::string line; std::getline(lines, line);) {
        line = trim(line);
        if (line.empty())
            continue;
        if (line.front() == '[') {
            flush();
            if (line == "[OptionBool]")              { have = true; cur.type = PostOption::Type::Bool; cur.max = 1.f; cur.step = 1.f; }
            else if (line == "[OptionRangeFloat]")   { have = true; cur.type = PostOption::Type::Float; }
            else if (line == "[OptionRangeInteger]") { have = true; cur.type = PostOption::Type::Int; cur.step = 1.f; }
            continue;
        }
        const auto eq = line.find('=');
        if (!have || eq == std::string::npos)
            continue;
        const std::string key   = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (value.find(',') != std::string::npos)
            vector_option = true;
        const auto number = [&]() -> float {
            if (value == "true")  return 1.f;
            if (value == "false") return 0.f;
            return std::strtof(value.c_str(), nullptr);
        };
        if (key == "GUIName")           cur.label = value;
        else if (key == "OptionName")   cur.name  = value;
        else if (key == "MinValue")     cur.min   = number();
        else if (key == "MaxValue")     cur.max   = number();
        else if (key == "StepAmount")   cur.step  = number();
        else if (key == "DefaultValue") cur.def   = number();
    }
    flush();
    for (auto& o : out) {
        if (o.label.empty()) o.label = o.name;
        if (o.step <= 0.f)   o.step = o.type == PostOption::Type::Float ? 0.01f : 1.f;
    }
    return out;
}

GLuint compile(GLenum type, const std::string& source, std::string& log) {
    const GLuint shader = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[2048];
        glGetShaderInfoLog(shader, sizeof(buf), nullptr, buf);
        log = buf;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

// ── Library ───────────────────────────────────────────────────────────────

fs::path PostLibrary::user_dir() {
    const fs::path dir = Config::data_dir() / "shaders";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

// Where slang presets are looked for: our own folder, then RetroArch's.
std::vector<fs::path> PostLibrary::slang_roots() {
    std::vector<fs::path> roots{user_dir() / "slang"};
    if (const char* home = std::getenv("HOME"))
        roots.push_back(fs::path(home) / ".config/retroarch/shaders/shaders_slang");
    roots.push_back("/usr/share/libretro/shaders/shaders_slang");
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& r : roots)
        if (fs::is_directory(r, ec))
            out.push_back(r);
    return out;
}

fs::path PostLibrary::slang_path(const std::string& name) {
    if (name.rfind("slang/", 0) != 0)
        return {};
    std::error_code ec;
    for (const auto& root : slang_roots()) {
        const fs::path file = root / (name.substr(6) + ".slangp");
        if (fs::is_regular_file(file, ec))
            return file;
    }
    return {};
}

fs::path PostLibrary::reshade_path(const std::string& name, std::string& technique) {
    technique.clear();
    if (name.rfind("reshade/", 0) != 0)
        return {};
    std::string rel = name.substr(8);
    std::error_code ec;
    for (int attempt = 0; attempt < 2; ++attempt) {
        for (const auto& root : ReshadeEffect::roots()) {
            const fs::path file = root / (rel + ".fx");
            if (fs::is_regular_file(file, ec))
                return file;
        }
        // "<file>:<technique>"
        const auto colon = rel.rfind(':');
        if (attempt > 0 || colon == std::string::npos)
            break;
        technique = rel.substr(colon + 1);
        rel = rel.substr(0, colon);
    }
    technique.clear();
    return {};
}

std::vector<std::string> PostLibrary::available() {
    std::vector<std::string> names;
    for (const auto& [name, src] : builtin_post_shaders())
        names.push_back(name);
    {
        std::error_code sec;
        for (const auto& root : slang_roots())
            for (const auto& e : fs::recursive_directory_iterator(root, sec)) {
                if (!e.is_regular_file(sec) || e.path().extension() != ".slangp")
                    continue;
                fs::path rel = fs::relative(e.path(), root, sec);
                rel.replace_extension();
                names.push_back("slang/" + rel.generic_string());
            }
    }
    {
        std::error_code rec;
        for (const auto& root : ReshadeEffect::roots())
            for (auto it = fs::recursive_directory_iterator(
                     root, fs::directory_options::follow_directory_symlink, rec);
                 !rec && it != fs::recursive_directory_iterator(); it.increment(rec)) {
                if (!it->is_regular_file(rec) || it->path().extension() != ".fx")
                    continue;
                // Lexically: pack folders are often symlinks, and the name
                // should follow the folders as the player sees them.
                fs::path rel = it->path().lexically_relative(root);
                rel.replace_extension();
                const std::string base = "reshade/" + rel.generic_string();
                const auto techniques = ReshadeEffect::techniques(it->path());
                if (techniques.size() <= 1)
                    names.push_back(base);
                else
                    for (const auto& t : techniques)
                        names.push_back(base + ":" + t);
            }
    }
    std::error_code ec;
    const fs::path dir = user_dir();
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto& e = *it;
        // ReShade packs keep their own .glsl-free folder; skip it quickly.
        if (e.is_directory(ec) && e.path() == dir / "reshade") {
            it.disable_recursion_pending();
            continue;
        }
        if (!e.is_regular_file(ec) || e.path().extension() != ".glsl")
            continue;
        fs::path rel = fs::relative(e.path(), dir, ec);
        rel.replace_extension();
        names.push_back(rel.generic_string());
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::string PostLibrary::load(const std::string& name) {
    // A user file overrides a built-in of the same name.
    std::ifstream file(user_dir() / (name + ".glsl"));
    if (file) {
        std::ostringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }
    const auto& builtin = builtin_post_shaders();
    const auto it = builtin.find(name);
    return it == builtin.end() ? std::string{} : it->second;
}

std::vector<PostOption> PostLibrary::options(const std::string& name) {
    if (const fs::path preset = slang_path(name); !preset.empty())
        return SlangPreset::read_parameters(preset);
    std::string technique;
    if (const fs::path effect = reshade_path(name, technique); !effect.empty())
        return ReshadeEffect::read_parameters(effect, technique);
    return parse_options(load(name));
}

// ── Chain ─────────────────────────────────────────────────────────────────

void PostChain::set_stages(std::vector<PostStage> stages) {
    m_stages = std::move(stages);
    m_dirty  = true;
}

void PostChain::set_option(std::size_t stage, const std::string& name, float value) {
    if (stage < m_stages.size())
        m_stages[stage].options[name] = value;
    if (stage < m_compiled.size()) {
        m_compiled[stage].values[name] = value;
        if (m_compiled[stage].slang)
            m_compiled[stage].slang->set_parameter(name, value);
        if (m_compiled[stage].reshade)
            m_compiled[stage].reshade->set_parameter(name, value);
    }
}

GLuint PostChain::build_program(const std::string& name, const std::string& source,
                                const std::vector<PostOption>& options) {
    std::string frag = kPrelude;
    for (const auto& o : options)
        frag += std::string("uniform ") +
                (o.type == PostOption::Type::Float ? "float " : "int ") + o.name + ";\n";
    frag += "#line 1\n";
    frag += source;

    std::string log;
    const GLuint vs = compile(GL_VERTEX_SHADER, kVertex, log);
    const GLuint fs_shader = vs ? compile(GL_FRAGMENT_SHADER, frag, log) : 0;
    if (!vs || !fs_shader) {
        if (vs) glDeleteShader(vs);
        m_errors += name + ": " + log + "\n";
        return 0;
    }
    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs_shader);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs_shader);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[1024];
        glGetProgramInfoLog(program, sizeof(buf), nullptr, buf);
        m_errors += name + ": " + buf + "\n";
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

void PostChain::rebuild() {
    for (auto& c : m_compiled) {
        if (c.program) glDeleteProgram(c.program);
        if (c.slang) c.slang->release();
        if (c.reshade) c.reshade->release();
    }
    m_compiled.clear();
    m_errors.clear();

    if (!m_downsample)
        m_downsample = build_program("downsample", kDownsample, {});
    if (!m_vao)
        glGenVertexArrays(1, &m_vao);
    if (!m_passthrough)
        m_passthrough = build_program("passthrough", kPassthrough, {});

    // One entry per configured stage, in order; a stage that fails to build
    // keeps its slot with program 0 and is skipped when rendering.
    for (const PostStage& stage : m_stages) {
        Compiled c;
        c.enabled = stage.enabled;
        if (const fs::path preset = PostLibrary::slang_path(stage.shader);
            stage.shader.rfind("slang/", 0) == 0) {
            auto slang = std::make_shared<SlangPreset>();
            std::string error;
            if (preset.empty()) {
                m_errors += stage.shader + ": preset not found\n";
            } else if (!slang->load(preset, error) || !slang->build(error)) {
                m_errors += stage.shader + ": " + error + "\n";
            } else {
                for (const auto& [name, value] : stage.options)
                    slang->set_parameter(name, value);
                c.options = slang->parameters();
                c.slang   = std::move(slang);
            }
            m_compiled.push_back(std::move(c));
            continue;
        }
        if (stage.shader.rfind("reshade/", 0) == 0) {
            std::string technique, error;
            const fs::path file = PostLibrary::reshade_path(stage.shader, technique);
            auto effect = std::make_shared<ReshadeEffect>();
            if (file.empty()) {
                m_errors += stage.shader + ": effect not found\n";
            } else if (!effect->load(file, technique, error)) {
                m_errors += stage.shader + ": " + error + "\n";
            } else {
                for (const auto& [name, value] : stage.options)
                    effect->set_parameter(name, value);
                c.options = effect->parameters();
                c.reshade = std::move(effect);
            }
            m_compiled.push_back(std::move(c));
            continue;
        }
        const std::string source = PostLibrary::load(stage.shader);
        if (source.empty()) {
            m_errors += stage.shader + ": shader not found\n";
        } else {
            c.options = parse_options(source);
            for (const auto& o : c.options) {
                const auto it = stage.options.find(o.name);
                c.values[o.name] = it != stage.options.end() ? it->second : o.def;
            }
            c.program = build_program(stage.shader, source, c.options);
        }
        m_compiled.push_back(std::move(c));
    }
    if (!m_errors.empty()) {
        m_new_errors = true;
        ds::common::log()->error("post-processing: {}", m_errors);
    }
    m_dirty = false;
}

void PostChain::ensure_target(Target& t, int w, int h) {
    if (t.fbo && t.w == w && t.h == h)
        return;
    if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
    if (t.tex) glDeleteTextures(1, &t.tex);
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    t.w = w;
    t.h = h;
}

void PostChain::draw(const Compiled* stage, GLuint input, const Frame& frame) {
    const GLuint program = stage ? stage->program : m_passthrough;
    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, input);
    const auto loc = [program](const char* name) { return glGetUniformLocation(program, name); };
    glUniform1i(loc("samp0"), 0);
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    glUniform2f(loc("u_window_size"), static_cast<float>(viewport[2]), static_cast<float>(viewport[3]));
    glUniform2f(loc("u_internal_size"), static_cast<float>(frame.source_w),
                static_cast<float>(frame.source_h));
    const float aspect = frame.h > 0 ? static_cast<float>(frame.w) / static_cast<float>(frame.h)
                                     : 4.f / 3.f;
    glUniform2f(loc("u_native_size"), 480.f * aspect, 480.f);
    glUniform2f(loc("u_origin"), static_cast<float>(viewport[0]), 0.f);
    glUniform1f(loc("u_fb_height"), static_cast<float>(viewport[1] + viewport[3]));
    glUniform1f(loc("u_time"), frame.time);
    if (stage) {
        for (const auto& o : stage->options) {
            const auto it = stage->values.find(o.name);
            const float v = it != stage->values.end() ? it->second : o.def;
            if (o.type == PostOption::Type::Float) glUniform1f(loc(o.name.c_str()), v);
            else                                   glUniform1i(loc(o.name.c_str()), static_cast<int>(v));
        }
    }
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// The game image reduced to 480 lines, for shaders that expect a console-
// resolution input. Returns the input unchanged when it is already small.
GLuint PostChain::native_input(const Frame& frame, int& w, int& h) {
    w = frame.source_w;
    h = frame.source_h;
    if (!m_native_input || !m_downsample || frame.source_h < 720)
        return frame.source;
    const int nh = 480;
    const int nw = std::max(1, static_cast<int>(std::lround(
        480.0 * static_cast<double>(frame.source_w) / static_cast<double>(frame.source_h))));
    ensure_target(m_native, nw, nh);
    glBindFramebuffer(GL_FRAMEBUFFER, m_native.fbo);
    glViewport(0, 0, nw, nh);
    Compiled down;
    down.program = m_downsample;
    draw(&down, frame.source, frame);
    down.program = 0;
    w = nw;
    h = nh;
    return m_native.tex;
}

void PostChain::render(const Frame& frame_in, const std::function<void()>& bind_output) {
    if (m_dirty)
        rebuild();
    ++m_frame_count;

    std::vector<Compiled*> active;
    if (m_enabled)
        for (auto& c : m_compiled)
            if (c.usable() && c.enabled)
                active.push_back(&c);

    Frame frame = frame_in;
    GLuint input = frame.source;
    int in_w = frame.source_w, in_h = frame.source_h;
    if (!active.empty()) {
        input = native_input(frame, in_w, in_h);
        frame.source_w = in_w;
        frame.source_h = in_h;
    }

    for (std::size_t i = 0; i < active.size(); ++i) {
        const bool last = i + 1 == active.size();
        Target* target = last ? nullptr : &m_targets[i % 2];
        if (target)
            ensure_target(*target, frame.w, frame.h);

        if (active[i]->slang) {
            SlangPreset::Frame sf;
            sf.input       = input;
            sf.input_w     = in_w;
            sf.input_h     = in_h;
            sf.x           = last ? frame.x : 0;
            sf.y           = last ? frame.y : 0;
            sf.w           = frame.w;
            sf.h           = frame.h;
            sf.frame_count = m_frame_count;
            const GLuint fbo = target ? target->fbo : 0;
            sf.bind_output = [&bind_output, fbo, last] {
                if (last) bind_output();
                else      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            };
            active[i]->slang->render(sf);
        } else if (active[i]->reshade) {
            ReshadeEffect::Frame rf;
            rf.input       = input;
            rf.input_w     = in_w;
            rf.input_h     = in_h;
            rf.x           = last ? frame.x : 0;
            rf.y           = last ? frame.y : 0;
            rf.w           = frame.w;
            rf.h           = frame.h;
            rf.frame_count = m_frame_count;
            const GLuint fbo = target ? target->fbo : 0;
            rf.bind_output = [&bind_output, fbo, last] {
                if (last) bind_output();
                else      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            };
            ReshadeEffect& effect = *active[i]->reshade;
            if (!effect.render(rf) || !effect.error().empty()) {
                // Built (or rebuilt for a new size) with problems: report once.
                const std::string line = effect.error();
                if (!line.empty() && m_errors.find(line) == std::string::npos) {
                    m_errors += line + "\n";
                    m_new_errors = true;
                    ds::common::log()->error("post-processing: {}", line);
                }
            }
        } else {
            if (last) {
                bind_output();
                glViewport(frame.x, frame.y, frame.w, frame.h);
            } else {
                glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
                glViewport(0, 0, frame.w, frame.h);
            }
            draw(active[i], input, frame);
        }
        if (target) {
            input = target->tex;
            in_w  = frame.w;
            in_h  = frame.h;
        }
    }
    if (active.empty()) {
        bind_output();
        glViewport(frame.x, frame.y, frame.w, frame.h);
        draw(nullptr, input, frame);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void PostChain::release() {
    for (auto& c : m_compiled) {
        if (c.program) glDeleteProgram(c.program);
        if (c.slang) c.slang->release();
        if (c.reshade) c.reshade->release();
    }
    m_compiled.clear();
    if (m_downsample) { glDeleteProgram(m_downsample); m_downsample = 0; }
    if (m_native.fbo) glDeleteFramebuffers(1, &m_native.fbo);
    if (m_native.tex) glDeleteTextures(1, &m_native.tex);
    m_native = {};
    for (auto& t : m_targets) {
        if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
        if (t.tex) glDeleteTextures(1, &t.tex);
        t = {};
    }
    if (m_passthrough) { glDeleteProgram(m_passthrough); m_passthrough = 0; }
    if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    m_dirty = true;
}

} // namespace ds::frontend
