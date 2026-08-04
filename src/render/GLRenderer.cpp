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

            program = create_program(
                read_text(shader_directory + "/video.vert"),
                read_text(shader_directory + "/video.frag"));

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
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

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
        state.readback_submit_ms = readback_submit.average();
        state.pbo_map_wait_ms = state.asynchronous_pbo
            ? pbo_map_wait.average()
            : 0.0;
    }

    GpuPerformanceSummary profiling_summary() {
        upload_timer.resolve_all();
        shader_timer.resolve_all();
        readback_timer.resolve_all();
        return GpuPerformanceSummary{upload_submit.average(),
                                     shader_timer.average_ms(),
                                     readback_submit.average(),
                                     pbo_map_wait.average()};
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

    void upload_frame(const VideoFrame& frame, bool asynchronous) {
        ensure_video_texture(frame);
        const std::size_t bytes = frame.rgb.size();
        const auto submit_start = std::chrono::steady_clock::now();
        upload_timer.begin();
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
        glUseProgram(program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glUniform1i(glGetUniformLocation(program, "u_video"), 0);
        glUniform1i(glGetUniformLocation(program, "u_filter_mode"),
                    static_cast<int>(state.filter));
        glUniform2f(glGetUniformLocation(program, "u_texel_size"),
                    1.0F / static_cast<float>(width),
                    1.0F / static_cast<float>(height));
        glUniform1f(glGetUniformLocation(program, "u_time"),
                    static_cast<float>(elapsed_seconds));
        glUniform1f(glGetUniformLocation(program, "u_effect_intensity"),
                    state.effect_intensity);
        glUniform1f(glGetUniformLocation(program, "u_edge_strength"),
                    state.edge_strength);
        glUniform1f(glGetUniformLocation(program, "u_vignette_strength"),
                    state.vignette_strength);
        glUniform1i(glGetUniformLocation(program, "u_split_view"),
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
        const VideoFrame metadata{frame.width,
                                  frame.height,
                                  frame.pts_seconds,
                                  {}};
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
        ImGui::Text("Readback submit %.3f ms | PBO wait %.3f ms",
                    state.readback_submit_ms,
                    state.pbo_map_wait_ms);
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
            if (export_texture != 0) glDeleteTextures(1, &export_texture);
            if (export_framebuffer != 0) {
                glDeleteFramebuffers(1, &export_framebuffer);
            }
            if (vbo != 0) glDeleteBuffers(1, &vbo);
            if (vao != 0) glDeleteVertexArrays(1, &vao);
            if (program != 0) glDeleteProgram(program);
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
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint texture = 0;
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
    int export_width = 0;
    int export_height = 0;
    int width = 0;
    int height = 0;
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
