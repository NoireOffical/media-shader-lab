#include "medialab/GLRenderer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_stdlib.h"

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#include <CoreVideo/CVPixelBufferIOSurface.h>
#include <IOSurface/IOSurface.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#else
#include <GL/glew.h>
#endif

namespace medialab {
namespace {

std::string read_text(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("could not open shader: " + path);
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

GLuint compile_shader(GLenum type, const std::string& source) {
    const GLuint shader = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success == GL_TRUE) {
        return shader;
    }
    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(length), '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    glDeleteShader(shader);
    throw std::runtime_error("shader compilation failed: " + log);
}

GLuint create_program(const std::string& vertex_source,
                      const std::string& fragment_source) {
    const GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
    const GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_tex_coord");
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint success = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (success == GL_TRUE) {
        return program;
    }
    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(length), '\0');
    glGetProgramInfoLog(program, length, nullptr, log.data());
    glDeleteProgram(program);
    throw std::runtime_error("shader program link failed: " + log);
}

std::string shader_variant(const std::string& source,
                           const std::string& define) {
    const std::size_t first_line_end = source.find('\n');
    if (source.rfind("#version", 0) != 0 ||
        first_line_end == std::string::npos) {
        throw std::runtime_error(
            "shader source must begin with a #version directive");
    }
    return source.substr(0, first_line_end + 1) +
           "#define " + define + " 1\n" +
           source.substr(first_line_end + 1);
}

class SampleWindow {
public:
    void add(double value) {
        samples_.push_back(value);
        if (samples_.size() > 120) {
            samples_.pop_front();
        }
    }

    double average() const noexcept {
        if (samples_.empty()) {
            return 0.0;
        }
        return std::accumulate(samples_.begin(), samples_.end(), 0.0) /
               static_cast<double>(samples_.size());
    }

    bool has_samples() const noexcept { return !samples_.empty(); }
    void clear() noexcept { samples_.clear(); }

private:
    std::deque<double> samples_;
};

class GpuTimerQueries {
public:
    void initialize() {
        glGenQueries(static_cast<GLsizei>(queries_.size()), queries_.data());
    }

    void cleanup() noexcept {
        if (queries_[0] != 0) {
            glDeleteQueries(static_cast<GLsizei>(queries_.size()), queries_.data());
            queries_.fill(0);
            pending_.fill(false);
        }
    }

    void collect() {
        for (std::size_t index = 0; index < queries_.size(); ++index) {
            if (!pending_[index]) {
                continue;
            }
            GLint available = GL_FALSE;
            glGetQueryObjectiv(queries_[index],
                               GL_QUERY_RESULT_AVAILABLE,
                               &available);
            if (available == GL_TRUE) {
                GLuint64 nanoseconds = 0;
                glGetQueryObjectui64v(queries_[index],
                                      GL_QUERY_RESULT,
                                      &nanoseconds);
                samples_.add(static_cast<double>(nanoseconds) / 1000000.0);
                pending_[index] = false;
            }
        }
    }

    void resolve_all() {
        for (std::size_t index = 0; index < queries_.size(); ++index) {
            if (!pending_[index]) {
                continue;
            }
            GLuint64 nanoseconds = 0;
            glGetQueryObjectui64v(queries_[index],
                                  GL_QUERY_RESULT,
                                  &nanoseconds);
            samples_.add(static_cast<double>(nanoseconds) / 1000000.0);
            pending_[index] = false;
        }
    }

    void begin() {
        collect();
        active_ = -1;
        for (std::size_t offset = 0; offset < queries_.size(); ++offset) {
            const std::size_t candidate = (next_ + offset) % queries_.size();
            if (!pending_[candidate]) {
                active_ = static_cast<int>(candidate);
                next_ = (candidate + 1) % queries_.size();
                glBeginQuery(GL_TIME_ELAPSED, queries_[candidate]);
                break;
            }
        }
    }

    void end() {
        if (active_ < 0) {
            return;
        }
        glEndQuery(GL_TIME_ELAPSED);
        pending_[static_cast<std::size_t>(active_)] = true;
        active_ = -1;
    }

    double average_ms() const noexcept { return samples_.average(); }
    bool has_samples() const noexcept { return samples_.has_samples(); }
    void clear_samples() noexcept { samples_.clear(); }

private:
    std::array<GLuint, 8> queries_{};
    std::array<bool, 8> pending_{};
    std::size_t next_ = 0;
    int active_ = -1;
    SampleWindow samples_;
};

}  // namespace

const char* filter_name(FilterMode mode) noexcept {
    switch (mode) {
        case FilterMode::Original: return "original";
        case FilterMode::Grayscale: return "grayscale";
        case FilterMode::Sepia: return "sepia";
        case FilterMode::Edge: return "edge";
        case FilterMode::Vignette: return "vignette";
    }
    return "unknown";
}

FilterMode parse_filter(const std::string& value) {
    if (value == "original") return FilterMode::Original;
    if (value == "grayscale") return FilterMode::Grayscale;
    if (value == "sepia") return FilterMode::Sepia;
    if (value == "edge") return FilterMode::Edge;
    if (value == "vignette") return FilterMode::Vignette;
    throw std::invalid_argument("unknown filter: " + value);
}

class GLRenderer::Impl {
public:
    Impl(int video_width,
         int video_height,
         const std::string& shader_directory,
         bool visible)
        : width(video_width), height(video_height) {
        if (glfwInit() != GLFW_TRUE) {
            throw std::runtime_error("GLFW initialization failed");
        }
        glfw_initialized = true;
        try {
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
            glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
#if defined(__APPLE__)
            glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
            const int window_width = std::max(width, 960);
            const int window_height = std::max(height, 600);
            window = glfwCreateWindow(
                window_width, window_height, "Media Shader Lab", nullptr, nullptr);
            if (window == nullptr) {
                throw std::runtime_error("could not create OpenGL window");
            }
            glfwMakeContextCurrent(window);
            glfwSwapInterval(1);

#if !defined(__APPLE__)
            glewExperimental = GL_TRUE;
            if (glewInit() != GLEW_OK) {
                throw std::runtime_error("GLEW initialization failed");
            }
#endif

            const std::string vertex_source =
                read_text(shader_directory + "/video.vert");
            const std::string fragment_source =
                read_text(shader_directory + "/video.frag");
            program = create_program(vertex_source, fragment_source);
#if defined(__APPLE__)
            hardware_program = create_program(
                vertex_source,
                shader_variant(fragment_source, "VIDEO_NV12"));
#endif

            const float vertices[] = {
                -1.0F, -1.0F, 0.0F, 1.0F,
                 1.0F, -1.0F, 1.0F, 1.0F,
                 1.0F,  1.0F, 1.0F, 0.0F,
                -1.0F, -1.0F, 0.0F, 1.0F,
                 1.0F,  1.0F, 1.0F, 0.0F,
                -1.0F,  1.0F, 0.0F, 0.0F,
            };

            glGenVertexArrays(1, &vao);
            glGenBuffers(1, &vbo);
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(1,
                                  2,
                                  GL_FLOAT,
                                  GL_FALSE,
                                  4 * sizeof(float),
                                  reinterpret_cast<void*>(2 * sizeof(float)));
            glEnableVertexAttribArray(1);

            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            const std::uint8_t placeholder[] = {0, 0, 0};
            glTexImage2D(GL_TEXTURE_2D,
                         0,
                         GL_RGB8,
                         1,
                         1,
                         0,
                         GL_RGB,
                         GL_UNSIGNED_BYTE,
                         placeholder);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

#if defined(__APPLE__)
            glGenTextures(1, &hardware_luma_texture);
            glBindTexture(GL_TEXTURE_RECTANGLE, hardware_luma_texture);
            configure_rectangle_texture();
            glGenTextures(1, &hardware_chroma_texture);
            glBindTexture(GL_TEXTURE_RECTANGLE, hardware_chroma_texture);
            configure_rectangle_texture();
#endif

            glGenBuffers(static_cast<GLsizei>(upload_pbos.size()),
                         upload_pbos.data());
            glGenBuffers(static_cast<GLsizei>(readback_pbos.size()),
                         readback_pbos.data());
            upload_timer.initialize();
            shader_timer.initialize();
            readback_timer.initialize();

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            imgui_context_created = true;
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            io.Fonts->AddFontDefaultVector();
            ImGui::StyleColorsDark();
            ImGuiStyle& style = ImGui::GetStyle();
            style.FontSizeBase = 16.0F;
            style.WindowRounding = 8.0F;
            style.FrameRounding = 5.0F;
            style.GrabRounding = 5.0F;

            if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
                throw std::runtime_error("Dear ImGui GLFW backend initialization failed");
            }
            imgui_glfw_initialized = true;
            if (!ImGui_ImplOpenGL3_Init("#version 150")) {
                throw std::runtime_error("Dear ImGui OpenGL backend initialization failed");
            }
            imgui_opengl_initialized = true;

            glfwSetWindowUserPointer(window, this);
            glfwSetDropCallback(window, [](GLFWwindow* source, int count, const char** paths) {
                auto* self = static_cast<Impl*>(glfwGetWindowUserPointer(source));
                if (self != nullptr && count > 0 && paths != nullptr && paths[0] != nullptr) {
                    self->pending_dropped_path = paths[0];
                }
            });
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~Impl() { cleanup(); }

    void collect_profiling(InteractiveState& state) {
        upload_timer.collect();
        shader_timer.collect();
        readback_timer.collect();
        state.upload_submit_ms = upload_submit.average();
        state.gpu_shader_ms = shader_timer.average_ms();
        if (readback_submit.has_samples()) {
            state.readback_metrics_available = true;
            state.readback_submit_ms = readback_submit.average();
            state.readback_gpu_available =
                readback_timer.has_samples() && readback_timer.average_ms() > 0.0;
            state.readback_gpu_ms = readback_timer.average_ms();
            state.readback_used_pbo = readback_used_pbo;
            state.pbo_wait_available =
                readback_used_pbo && pbo_map_wait.has_samples();
            state.pbo_map_wait_ms = pbo_map_wait.average();
        }
    }

    void reset_profiling() {
        upload_timer.resolve_all();
        shader_timer.resolve_all();
        readback_timer.resolve_all();
        upload_timer.clear_samples();
        shader_timer.clear_samples();
        readback_timer.clear_samples();
        upload_submit.clear();
        readback_submit.clear();
        pbo_map_wait.clear();
        readback_used_pbo = false;
    }

    GpuPerformanceSummary profiling_summary() {
        upload_timer.resolve_all();
        shader_timer.resolve_all();
        readback_timer.resolve_all();
        GpuPerformanceSummary summary;
        summary.upload_submit_ms = upload_submit.average();
        summary.shader_ms = shader_timer.average_ms();
        summary.readback_submit_ms = readback_submit.average();
        summary.readback_gpu_ms = readback_timer.average_ms();
        summary.pbo_map_wait_ms = pbo_map_wait.average();
        summary.readback_sampled = readback_submit.has_samples();
        summary.readback_gpu_sampled =
            readback_timer.has_samples() && readback_timer.average_ms() > 0.0;
        summary.pbo_wait_sampled = pbo_map_wait.has_samples();
        summary.readback_used_pbo = readback_used_pbo;
        return summary;
    }

    void ensure_video_texture(const VideoFrame& frame) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        if (!texture_initialized || frame.width != width ||
            frame.height != height) {
            width = frame.width;
            height = frame.height;
            glTexImage2D(GL_TEXTURE_2D,
                         0,
                         GL_RGB8,
                         frame.width,
                         frame.height,
                         0,
                         GL_RGB,
                         GL_UNSIGNED_BYTE,
                         nullptr);
            texture_initialized = true;
            upload_index = 0;
        }
    }

#if defined(__APPLE__)
    static void configure_rectangle_texture() {
        glTexParameteri(GL_TEXTURE_RECTANGLE,
                        GL_TEXTURE_MIN_FILTER,
                        GL_LINEAR);
        glTexParameteri(GL_TEXTURE_RECTANGLE,
                        GL_TEXTURE_MAG_FILTER,
                        GL_LINEAR);
        glTexParameteri(GL_TEXTURE_RECTANGLE,
                        GL_TEXTURE_WRAP_S,
                        GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_RECTANGLE,
                        GL_TEXTURE_WRAP_T,
                        GL_CLAMP_TO_EDGE);
    }

    void bind_iosurface_plane(CVPixelBufferRef pixel_buffer,
                              GLuint texture_id,
                              std::size_t plane,
                              GLenum internal_format,
                              GLenum format) {
        auto surface = static_cast<IOSurfaceRef>(
            CVPixelBufferGetIOSurface(pixel_buffer));
        if (surface == nullptr) {
            throw std::runtime_error(
                "VideoToolbox frame is not backed by an IOSurface; "
                "retry with --no-zero-copy");
        }
        const auto plane_width = static_cast<GLsizei>(
            CVPixelBufferGetWidthOfPlane(pixel_buffer, plane));
        const auto plane_height = static_cast<GLsizei>(
            CVPixelBufferGetHeightOfPlane(pixel_buffer, plane));
        glBindTexture(GL_TEXTURE_RECTANGLE, texture_id);
        const CGLError result = CGLTexImageIOSurface2D(
            CGLGetCurrentContext(),
            GL_TEXTURE_RECTANGLE,
            internal_format,
            plane_width,
            plane_height,
            format,
            GL_UNSIGNED_BYTE,
            surface,
            static_cast<GLuint>(plane));
        if (result != kCGLNoError) {
            throw std::runtime_error(
                std::string("could not bind VideoToolbox IOSurface plane: ") +
                CGLErrorString(result) + "; retry with --no-zero-copy");
        }
    }

    void upload_hardware_frame(const VideoFrame& frame) {
        auto pixel_buffer = static_cast<CVPixelBufferRef>(
            frame.hardware_surface.get());
        if (pixel_buffer == nullptr ||
            CVPixelBufferGetPlaneCount(pixel_buffer) != 2) {
            throw std::runtime_error(
                "invalid VideoToolbox NV12 surface; retry with --no-zero-copy");
        }
        glActiveTexture(GL_TEXTURE1);
        bind_iosurface_plane(pixel_buffer,
                             hardware_luma_texture,
                             0,
                             GL_R8,
                             GL_RED);
        glActiveTexture(GL_TEXTURE2);
        bind_iosurface_plane(pixel_buffer,
                             hardware_chroma_texture,
                             1,
                             GL_RG8,
                             GL_RG);
        hardware_chroma_width = static_cast<int>(
            CVPixelBufferGetWidthOfPlane(pixel_buffer, 1));
        hardware_chroma_height = static_cast<int>(
            CVPixelBufferGetHeightOfPlane(pixel_buffer, 1));
        width = frame.width;
        height = frame.height;
        hardware_format = frame.hardware_format;
        hardware_color_matrix = frame.color_matrix;
        using_hardware_texture = true;
    }
#endif

    void upload_frame(const VideoFrame& frame, bool asynchronous) {
        const auto submit_start = std::chrono::steady_clock::now();
        upload_timer.begin();
#if defined(__APPLE__)
        if (frame.has_hardware_surface()) {
            upload_hardware_frame(frame);
            upload_timer.end();
            upload_submit.add(std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - submit_start)
                                  .count());
            return;
        }
#endif
        if (frame.rgb.size() != static_cast<std::size_t>(frame.width) *
                                    static_cast<std::size_t>(frame.height) * 3U) {
            upload_timer.end();
            throw std::runtime_error("video frame has no readable RGB pixels");
        }
        using_hardware_texture = false;
        ensure_video_texture(frame);
        const std::size_t bytes = frame.rgb.size();
        if (asynchronous) {
            const GLuint pbo = upload_pbos[upload_index];
            upload_index = (upload_index + 1) % upload_pbos.size();
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
            glBufferData(GL_PIXEL_UNPACK_BUFFER,
                         static_cast<GLsizeiptr>(bytes),
                         nullptr,
                         GL_STREAM_DRAW);
            void* mapped = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER,
                                            0,
                                            static_cast<GLsizeiptr>(bytes),
                                            GL_MAP_WRITE_BIT |
                                                GL_MAP_INVALIDATE_BUFFER_BIT);
            if (mapped != nullptr) {
                std::memcpy(mapped, frame.rgb.data(), bytes);
                glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
                glTexSubImage2D(GL_TEXTURE_2D,
                                0,
                                0,
                                0,
                                frame.width,
                                frame.height,
                                GL_RGB,
                                GL_UNSIGNED_BYTE,
                                nullptr);
            } else {
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                glTexSubImage2D(GL_TEXTURE_2D,
                                0,
                                0,
                                0,
                                frame.width,
                                frame.height,
                                GL_RGB,
                                GL_UNSIGNED_BYTE,
                                frame.rgb.data());
            }
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        } else {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            glTexSubImage2D(GL_TEXTURE_2D,
                            0,
                            0,
                            0,
                            frame.width,
                            frame.height,
                            GL_RGB,
                            GL_UNSIGNED_BYTE,
                            frame.rgb.data());
        }
        upload_timer.end();
        upload_submit.add(std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - submit_start)
                              .count());
    }

    void draw_video(const InteractiveState& state,
                    double elapsed_seconds,
                    bool split_view) {
        shader_timer.begin();
        GLuint active_program = program;
#if defined(__APPLE__)
        if (using_hardware_texture) {
            active_program = hardware_program;
        }
#endif
        glUseProgram(active_program);
#if defined(__APPLE__)
        if (using_hardware_texture) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_RECTANGLE, hardware_luma_texture);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_RECTANGLE, hardware_chroma_texture);
            glUniform1i(glGetUniformLocation(active_program, "u_video_y"), 1);
            glUniform1i(glGetUniformLocation(active_program, "u_video_uv"), 2);
            glUniform1i(
                glGetUniformLocation(active_program, "u_video_full_range"),
                hardware_format == HardwareSurfaceFormat::Nv12FullRange
                    ? GL_TRUE
                    : GL_FALSE);
            glUniform1i(glGetUniformLocation(active_program, "u_yuv_matrix"),
                        static_cast<int>(hardware_color_matrix));
            glUniform2f(glGetUniformLocation(active_program, "u_video_size"),
                        static_cast<float>(width),
                        static_cast<float>(height));
            glUniform2f(glGetUniformLocation(active_program, "u_chroma_size"),
                        static_cast<float>(hardware_chroma_width),
                        static_cast<float>(hardware_chroma_height));
        } else
#endif
        {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture);
            glUniform1i(glGetUniformLocation(active_program, "u_video"), 0);
        }
        glUniform1i(glGetUniformLocation(active_program, "u_filter_mode"),
                    static_cast<int>(state.filter));
        glUniform2f(glGetUniformLocation(active_program, "u_texel_size"),
                    1.0F / static_cast<float>(width),
                    1.0F / static_cast<float>(height));
        glUniform1f(glGetUniformLocation(active_program, "u_time"),
                    static_cast<float>(elapsed_seconds));
        glUniform1f(glGetUniformLocation(active_program, "u_effect_intensity"),
                    state.effect_intensity);
        glUniform1f(glGetUniformLocation(active_program, "u_edge_strength"),
                    state.edge_strength);
        glUniform1f(glGetUniformLocation(active_program, "u_vignette_strength"),
                    state.vignette_strength);
        glUniform1i(glGetUniformLocation(active_program, "u_split_view"),
                    split_view ? GL_TRUE : GL_FALSE);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        shader_timer.end();
    }

    void ensure_readback_buffers(int frame_width, int frame_height) {
        if (readback_width == frame_width && readback_height == frame_height) {
            return;
        }
        readback_width = frame_width;
        readback_height = frame_height;
        readback_bytes = static_cast<std::size_t>(frame_width) *
                         static_cast<std::size_t>(frame_height) * 3U;
        for (const GLuint pbo : readback_pbos) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
            glBufferData(GL_PIXEL_PACK_BUFFER,
                         static_cast<GLsizeiptr>(readback_bytes),
                         nullptr,
                         GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        readback_pending.fill(false);
        readback_index = 0;
    }

    static void copy_bottom_up(const std::uint8_t* bottom_up,
                               const VideoFrame& metadata,
                               VideoFrame& output) {
        const std::size_t row_bytes =
            static_cast<std::size_t>(metadata.width) * 3U;
        output.width = metadata.width;
        output.height = metadata.height;
        output.pts_seconds = metadata.pts_seconds;
        output.rgb.resize(row_bytes * static_cast<std::size_t>(metadata.height));
        for (int row = 0; row < metadata.height; ++row) {
            const std::size_t source_offset =
                static_cast<std::size_t>(metadata.height - 1 - row) * row_bytes;
            const std::size_t destination_offset =
                static_cast<std::size_t>(row) * row_bytes;
            std::copy_n(bottom_up + source_offset,
                        row_bytes,
                        output.rgb.data() + destination_offset);
        }
    }

    bool map_readback(std::size_t index, VideoFrame& output) {
        if (!readback_pending[index]) {
            return false;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, readback_pbos[index]);
        const auto wait_start = std::chrono::steady_clock::now();
        const auto* mapped = static_cast<const std::uint8_t*>(
            glMapBufferRange(GL_PIXEL_PACK_BUFFER,
                             0,
                             static_cast<GLsizeiptr>(readback_bytes),
                             GL_MAP_READ_BIT));
        const auto wait_end = std::chrono::steady_clock::now();
        pbo_map_wait.add(std::chrono::duration<double, std::milli>(
                             wait_end - wait_start).count());
        if (mapped == nullptr) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            throw std::runtime_error("could not map asynchronous readback PBO");
        }
        copy_bottom_up(mapped, readback_metadata[index], output);
        const GLboolean unmap_result = glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        if (unmap_result != GL_TRUE) {
            throw std::runtime_error("GPU invalidated asynchronous readback data");
        }
        readback_pending[index] = false;
        return true;
    }

    bool readback_frame(const VideoFrame& frame,
                        bool asynchronous,
                        VideoFrame& output) {
        readback_used_pbo = asynchronous;
        VideoFrame metadata;
        metadata.width = frame.width;
        metadata.height = frame.height;
        metadata.pts_seconds = frame.pts_seconds;
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        readback_timer.begin();
        if (!asynchronous) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            std::vector<std::uint8_t> bottom_up(
                static_cast<std::size_t>(frame.width) *
                static_cast<std::size_t>(frame.height) * 3U);
            const auto submit_start = std::chrono::steady_clock::now();
            glReadPixels(0,
                         0,
                         frame.width,
                         frame.height,
                         GL_RGB,
                         GL_UNSIGNED_BYTE,
                         bottom_up.data());
            readback_submit.add(std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() -
                                    submit_start).count());
            readback_timer.end();
            copy_bottom_up(bottom_up.data(), metadata, output);
            return true;
        }

        ensure_readback_buffers(frame.width, frame.height);
        const std::size_t current = readback_index;
        const std::size_t previous = (current + 1) % readback_pbos.size();
        glBindBuffer(GL_PIXEL_PACK_BUFFER, readback_pbos[current]);
        const auto submit_start = std::chrono::steady_clock::now();
        glReadPixels(0,
                     0,
                     frame.width,
                     frame.height,
                     GL_RGB,
                     GL_UNSIGNED_BYTE,
                     nullptr);
        readback_submit.add(std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() -
                                submit_start).count());
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        readback_timer.end();
        readback_metadata[current] = metadata;
        readback_pending[current] = true;
        readback_index = previous;
        return map_readback(previous, output);
    }

    bool flush_readback(VideoFrame& output) {
        for (std::size_t offset = 0; offset < readback_pbos.size(); ++offset) {
            const std::size_t index = (readback_index + offset) %
                                      readback_pbos.size();
            if (map_readback(index, output)) {
                return true;
            }
        }
        return false;
    }

    void begin_control_frame(InteractiveState& state) {
        collect_profiling(state);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantCaptureKeyboard) {
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
            if (glfwGetKey(window, GLFW_KEY_0) == GLFW_PRESS) state.filter = FilterMode::Original;
            if (glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS) state.filter = FilterMode::Grayscale;
            if (glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS) state.filter = FilterMode::Sepia;
            if (glfwGetKey(window, GLFW_KEY_3) == GLFW_PRESS) state.filter = FilterMode::Edge;
            if (glfwGetKey(window, GLFW_KEY_4) == GLFW_PRESS) state.filter = FilterMode::Vignette;
            const bool space_pressed = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
            if (space_pressed && !space_was_pressed) {
                state.paused = !state.paused;
            }
            space_was_pressed = space_pressed;
        } else {
            space_was_pressed = false;
        }

        ImGui::SetNextWindowPos(ImVec2(16.0F, 16.0F), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(430.0F, 720.0F), ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.93F);
        ImGui::Begin("Media Shader Lab Controls", nullptr, ImGuiWindowFlags_NoCollapse);

        ImGui::TextUnformatted("Video source");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint(
            "##video_path", "Drop a video or enter its path", &state.input_path);
        if (ImGui::Button("Load video")) {
            state.load_requested = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Drag and drop is supported");
        static const char* decoder_labels[] = {
            "FFmpeg software", "VideoToolbox hardware"};
        ImGui::TextUnformatted("Decoder");
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::Combo("##decoder_backend",
                         &state.decoder_backend,
                         decoder_labels,
                         2)) {
            state.load_requested = true;
        }
        ImGui::TextDisabled("Active: %s", state.active_decoder.c_str());

        ImGui::SeparatorText("Playback");
        if (ImGui::Button(state.paused ? "Play" : "Pause", ImVec2(88.0F, 0.0F))) {
            state.paused = !state.paused;
        }
        ImGui::SameLine();
        ImGui::Checkbox("Split comparison", &state.split_view);

        float progress = static_cast<float>(state.progress_seconds);
        const float duration = std::max(0.001F, static_cast<float>(state.duration_seconds));
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::SliderFloat("##progress", &progress, 0.0F, duration, "%.2f s")) {
            state.progress_seconds = progress;
            state.seek_target_seconds = progress;
            state.seek_requested = true;
        }
        ImGui::Text("%.2f / %.2f seconds", state.progress_seconds, state.duration_seconds);

        ImGui::SeparatorText("Shader controls");
        static const char* filter_labels[] = {
            "Original", "Grayscale", "Sepia", "Edge", "Vignette"};
        int filter_index = static_cast<int>(state.filter);
        ImGui::TextUnformatted("Filter");
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::Combo("##filter", &filter_index, filter_labels, 5)) {
            state.filter = static_cast<FilterMode>(filter_index);
        }
        ImGui::TextUnformatted("Effect intensity");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::SliderFloat(
            "##effect_intensity", &state.effect_intensity, 0.0F, 1.0F, "%.2f");
        if (state.filter == FilterMode::Edge) {
            ImGui::TextUnformatted("Edge strength");
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::SliderFloat(
                "##edge_strength", &state.edge_strength, 0.2F, 5.0F, "%.2f");
        }
        if (state.filter == FilterMode::Vignette) {
            ImGui::TextUnformatted("Vignette strength");
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::SliderFloat(
                "##vignette_strength",
                &state.vignette_strength,
                0.0F,
                1.0F,
                "%.2f");
        }

        ImGui::SeparatorText("Live performance");
        ImGui::Checkbox("Asynchronous PBO transfers",
                        &state.asynchronous_pbo);
        ImGui::Text("Playback FPS (recent 1 s) %.1f", state.wall_fps);
        ImGui::Text("Decode avg %.2f ms", state.average_decode_ms);
        ImGui::Text("Render avg %.2f ms", state.average_render_ms);
        ImGui::Text("Pipeline P95 %.2f ms", state.p95_pipeline_ms);
        ImGui::Text("Upload submit %.3f ms | Shader GPU %.3f ms",
                    state.upload_submit_ms,
                    state.gpu_shader_ms);
        if (!state.readback_metrics_available) {
            ImGui::TextDisabled("Readback: N/A (measured during export)");
        } else {
            const char* readback_label = state.export_in_progress
                ? "Export readback"
                : "Last export readback";
            if (state.readback_gpu_available) {
                ImGui::Text("%s: submit %.3f ms | GPU %.3f ms",
                            readback_label,
                            state.readback_submit_ms,
                            state.readback_gpu_ms);
            } else if (state.export_in_progress) {
                ImGui::Text("%s: submit %.3f ms | GPU pending",
                            readback_label,
                            state.readback_submit_ms);
            } else {
                ImGui::Text("%s: submit %.3f ms | GPU N/A",
                            readback_label,
                            state.readback_submit_ms);
            }
            if (!state.readback_used_pbo) {
                ImGui::TextDisabled("PBO map wait: N/A (synchronous readback)");
            } else if (state.pbo_wait_available) {
                ImGui::Text("PBO map wait %.3f ms", state.pbo_map_wait_ms);
            } else if (state.export_in_progress) {
                ImGui::TextDisabled("PBO map wait: pending");
            } else {
                ImGui::TextDisabled("PBO map wait: N/A (no completed sample)");
            }
        }
        if (!state.pipeline_history_ms.empty()) {
            ImGui::PlotLines("##pipeline_history",
                             state.pipeline_history_ms.data(),
                             static_cast<int>(state.pipeline_history_ms.size()),
                             0,
                             "Pipeline latency (ms)",
                             0.0F,
                             40.0F,
                             ImVec2(-1.0F, 70.0F));
        }

        ImGui::SeparatorText("H.265 export");
        ImGui::BeginDisabled(state.export_in_progress);
        ImGui::TextUnformatted("Output path");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputText("##export_path", &state.export_path);
        static const char* encoder_labels[] = {
            "libx265 (software)", "VideoToolbox (hardware)"};
        ImGui::TextUnformatted("Encoder");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::Combo("##export_encoder",
                     &state.export_encoder,
                     encoder_labels,
                     2);
        if (state.export_encoder == 0) {
            ImGui::SliderInt("CRF", &state.export_crf, 0, 51);
            ImGui::TextUnformatted("Preset");
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::InputText("##export_preset", &state.export_preset);
        } else {
            ImGui::SliderInt("Bitrate (kbps)",
                             &state.export_bitrate_kbps,
                             500,
                             30000);
        }
        ImGui::SliderInt("GOP frames", &state.export_gop_size, 0, 300);
        ImGui::Checkbox("Copy source audio", &state.export_copy_audio);
        ImGui::Checkbox("Write PSNR/SSIM/VMAF report",
                        &state.export_quality_report);
        if (ImGui::Button("Export processed video", ImVec2(-1.0F, 0.0F))) {
            state.export_requested = true;
        }
        ImGui::EndDisabled();
        if (state.export_in_progress) {
            ImGui::ProgressBar(state.export_progress, ImVec2(-1.0F, 0.0F));
            if (ImGui::Button("Cancel export", ImVec2(-1.0F, 0.0F))) {
                state.export_cancel_requested = true;
            }
        }

        if (!state.status_message.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", state.status_message.c_str());
        }
        ImGui::TextDisabled("Space: play/pause | 0-4: filters | Esc: exit");
        ImGui::End();
    }

    void render_control_frame() const {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    void save_ppm(const std::string& path) const {
        int framebuffer_width = 0;
        int framebuffer_height = 0;
        glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
        const std::size_t row_bytes = static_cast<std::size_t>(framebuffer_width) * 3U;
        std::vector<unsigned char> pixels(
            row_bytes * static_cast<std::size_t>(framebuffer_height));

        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0,
                     0,
                     framebuffer_width,
                     framebuffer_height,
                     GL_RGB,
                     GL_UNSIGNED_BYTE,
                     pixels.data());

        std::ofstream output(path, std::ios::binary);
        if (!output) {
            throw std::runtime_error("could not open snapshot output: " + path);
        }
        output << "P6\n" << framebuffer_width << ' ' << framebuffer_height << "\n255\n";
        for (int row = framebuffer_height - 1; row >= 0; --row) {
            const auto offset = static_cast<std::size_t>(row) * row_bytes;
            output.write(reinterpret_cast<const char*>(pixels.data() + offset),
                         static_cast<std::streamsize>(row_bytes));
        }
        if (!output) {
            throw std::runtime_error("failed while writing snapshot: " + path);
        }
    }

    void cleanup() noexcept {
        if (window != nullptr) {
            glfwMakeContextCurrent(window);
            if (imgui_opengl_initialized) {
                ImGui_ImplOpenGL3_Shutdown();
                imgui_opengl_initialized = false;
            }
            if (imgui_glfw_initialized) {
                ImGui_ImplGlfw_Shutdown();
                imgui_glfw_initialized = false;
            }
            if (imgui_context_created) {
                ImGui::DestroyContext();
                imgui_context_created = false;
            }
            upload_timer.cleanup();
            shader_timer.cleanup();
            readback_timer.cleanup();
            if (upload_pbos[0] != 0) {
                glDeleteBuffers(static_cast<GLsizei>(upload_pbos.size()),
                                upload_pbos.data());
                upload_pbos.fill(0);
            }
            if (readback_pbos[0] != 0) {
                glDeleteBuffers(static_cast<GLsizei>(readback_pbos.size()),
                                readback_pbos.data());
                readback_pbos.fill(0);
            }
            if (texture != 0) glDeleteTextures(1, &texture);
#if defined(__APPLE__)
            if (hardware_luma_texture != 0) {
                glDeleteTextures(1, &hardware_luma_texture);
            }
            if (hardware_chroma_texture != 0) {
                glDeleteTextures(1, &hardware_chroma_texture);
            }
#endif
            if (export_texture != 0) glDeleteTextures(1, &export_texture);
            if (export_framebuffer != 0) {
                glDeleteFramebuffers(1, &export_framebuffer);
            }
            if (vbo != 0) glDeleteBuffers(1, &vbo);
            if (vao != 0) glDeleteVertexArrays(1, &vao);
            if (program != 0) glDeleteProgram(program);
#if defined(__APPLE__)
            if (hardware_program != 0) glDeleteProgram(hardware_program);
#endif
            glfwDestroyWindow(window);
            window = nullptr;
        }
        if (glfw_initialized) {
            glfwTerminate();
            glfw_initialized = false;
        }
    }

    GLFWwindow* window = nullptr;
    GLuint program = 0;
#if defined(__APPLE__)
    GLuint hardware_program = 0;
#endif
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint texture = 0;
#if defined(__APPLE__)
    GLuint hardware_luma_texture = 0;
    GLuint hardware_chroma_texture = 0;
#endif
    GLuint export_texture = 0;
    GLuint export_framebuffer = 0;
    std::array<GLuint, 2> upload_pbos{};
    std::array<GLuint, 2> readback_pbos{};
    std::array<bool, 2> readback_pending{};
    std::array<VideoFrame, 2> readback_metadata{};
    std::size_t upload_index = 0;
    std::size_t readback_index = 0;
    std::size_t readback_bytes = 0;
    int readback_width = 0;
    int readback_height = 0;
    GpuTimerQueries upload_timer;
    GpuTimerQueries shader_timer;
    GpuTimerQueries readback_timer;
    SampleWindow pbo_map_wait;
    SampleWindow upload_submit;
    SampleWindow readback_submit;
    bool readback_used_pbo = false;
    int export_width = 0;
    int export_height = 0;
    int width = 0;
    int height = 0;
    int hardware_chroma_width = 1;
    int hardware_chroma_height = 1;
    HardwareSurfaceFormat hardware_format = HardwareSurfaceFormat::None;
    VideoColorMatrix hardware_color_matrix = VideoColorMatrix::Bt601;
    bool using_hardware_texture = false;
    bool texture_initialized = false;
    bool glfw_initialized = false;
    bool imgui_context_created = false;
    bool imgui_glfw_initialized = false;
    bool imgui_opengl_initialized = false;
    bool space_was_pressed = false;
    std::string pending_dropped_path;
};

GLRenderer::GLRenderer(int video_width,
                       int video_height,
                       const std::string& shader_directory,
                       bool visible)
    : impl_(std::make_unique<Impl>(
          video_width, video_height, shader_directory, visible)) {}

GLRenderer::~GLRenderer() = default;

bool GLRenderer::should_close() const {
    return glfwWindowShouldClose(impl_->window) == GLFW_TRUE;
}

void GLRenderer::poll_events() { glfwPollEvents(); }

void GLRenderer::render(const VideoFrame& frame,
                        InteractiveState& state,
                        double elapsed_seconds,
                        const std::string& snapshot_output,
                        bool include_controls_in_snapshot) {
    const bool show_controls =
        snapshot_output.empty() || include_controls_in_snapshot;
    if (show_controls) {
        impl_->begin_control_frame(state);
    }

    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetFramebufferSize(impl_->window, &framebuffer_width, &framebuffer_height);
    glViewport(0, 0, framebuffer_width, framebuffer_height);
    glClearColor(0.03F, 0.03F, 0.04F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    impl_->upload_frame(frame, state.asynchronous_pbo);
    impl_->draw_video(state, elapsed_seconds, state.split_view);
    if (!snapshot_output.empty() && !include_controls_in_snapshot) {
        impl_->save_ppm(snapshot_output);
    }
    if (show_controls) {
        impl_->render_control_frame();
    }
    if (!snapshot_output.empty() && include_controls_in_snapshot) {
        impl_->save_ppm(snapshot_output);
    }
    glfwSwapBuffers(impl_->window);
}

bool GLRenderer::process(const VideoFrame& frame,
                         const InteractiveState& state,
                         double elapsed_seconds,
                         VideoFrame& output) {
    glfwMakeContextCurrent(impl_->window);
    impl_->upload_frame(frame, state.asynchronous_pbo);

    if (impl_->export_framebuffer == 0) {
        glGenFramebuffers(1, &impl_->export_framebuffer);
        glGenTextures(1, &impl_->export_texture);
    }
    if (impl_->export_width != frame.width ||
        impl_->export_height != frame.height) {
        impl_->export_width = frame.width;
        impl_->export_height = frame.height;
        glBindTexture(GL_TEXTURE_2D, impl_->export_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGB8,
                     frame.width,
                     frame.height,
                     0,
                     GL_RGB,
                     GL_UNSIGNED_BYTE,
                     nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, impl_->export_framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,
                               impl_->export_texture,
                               0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            throw std::runtime_error("could not create export framebuffer");
        }
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, impl_->export_framebuffer);
    }

    glViewport(0, 0, frame.width, frame.height);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    impl_->draw_video(state, elapsed_seconds, false);
    const bool output_ready = impl_->readback_frame(
        frame, state.asynchronous_pbo, output);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return output_ready;
}

bool GLRenderer::flush_process(VideoFrame& output) {
    glfwMakeContextCurrent(impl_->window);
    return impl_->flush_readback(output);
}

void GLRenderer::reset_profiling() {
    glfwMakeContextCurrent(impl_->window);
    impl_->reset_profiling();
}

GpuPerformanceSummary GLRenderer::profiling_summary() {
    glfwMakeContextCurrent(impl_->window);
    return impl_->profiling_summary();
}

std::string GLRenderer::take_dropped_path() {
    std::string result = std::move(impl_->pending_dropped_path);
    impl_->pending_dropped_path.clear();
    return result;
}

}  // namespace medialab
