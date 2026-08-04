cmake_minimum_required(VERSION 3.20)

project(MediaShaderLab VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswscale
)
pkg_check_modules(LIBVMAF QUIET IMPORTED_TARGET libvmaf)
find_package(glfw3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

if(APPLE)
    find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
    find_library(IOSURFACE_FRAMEWORK IOSurface REQUIRED)
endif()

if(NOT APPLE)
    find_package(GLEW REQUIRED)
endif()

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/cpp
)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)
target_compile_definitions(imgui PRIVATE GL_SILENCE_DEPRECATION)

add_library(media_core
    src/core/Metrics.cpp
    src/core/QualityMetrics.cpp
)
target_include_directories(media_core PUBLIC include)
if(LIBVMAF_FOUND)
    target_link_libraries(media_core PUBLIC PkgConfig::LIBVMAF)
    target_compile_definitions(media_core PUBLIC MEDIA_HAS_VMAF=1)
    message(STATUS "libvmaf ${LIBVMAF_VERSION} enabled")
else()
    message(WARNING "libvmaf was not found; VMAF reports will be disabled")
endif()

add_executable(media_shader_lab
    src/main.cpp
    src/media/VideoDecoder.cpp
    src/media/VideoEncoder.cpp
    src/render/GLRenderer.cpp
)
target_include_directories(media_shader_lab PRIVATE include)
target_link_libraries(media_shader_lab PRIVATE
    media_core
    imgui
    PkgConfig::FFMPEG
    glfw
    OpenGL::GL
)

if(APPLE)
    target_link_libraries(media_shader_lab PRIVATE
        ${COREVIDEO_FRAMEWORK}
        ${IOSURFACE_FRAMEWORK}
    )
endif()

if(NOT APPLE)
    target_link_libraries(media_shader_lab PRIVATE GLEW::GLEW)
endif()

target_compile_definitions(media_shader_lab PRIVATE
    GL_SILENCE_DEPRECATION
    MEDIA_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(media_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(media_shader_lab PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(imgui PRIVATE -Wall -Wextra -Wpedantic)
endif()

file(COPY shaders DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

include(CTest)
if(BUILD_TESTING)
    add_executable(metrics_test tests/MetricsTest.cpp)
    target_link_libraries(metrics_test PRIVATE media_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(metrics_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME metrics_test COMMAND metrics_test)

    add_executable(quality_metrics_test tests/QualityMetricsTest.cpp)
    target_link_libraries(quality_metrics_test PRIVATE media_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(quality_metrics_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME quality_metrics_test COMMAND quality_metrics_test)

    add_executable(decoder_seek_test
        tests/VideoDecoderIntegrationTest.cpp
        src/media/VideoDecoder.cpp
    )
    target_include_directories(decoder_seek_test PRIVATE include)
    target_link_libraries(decoder_seek_test PRIVATE PkgConfig::FFMPEG)
    if(APPLE)
        target_link_libraries(decoder_seek_test PRIVATE
            ${COREVIDEO_FRAMEWORK}
            ${IOSURFACE_FRAMEWORK}
        )
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(decoder_seek_test PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(decoder_seek_test PRIVATE -UNDEBUG)
    endif()

    add_executable(encoder_integration_test
        tests/VideoEncoderIntegrationTest.cpp
        src/media/VideoDecoder.cpp
        src/media/VideoEncoder.cpp
    )
    target_include_directories(encoder_integration_test PRIVATE include)
    target_link_libraries(encoder_integration_test PRIVATE PkgConfig::FFMPEG)
    if(APPLE)
        target_link_libraries(encoder_integration_test PRIVATE
            ${COREVIDEO_FRAMEWORK}
            ${IOSURFACE_FRAMEWORK}
        )
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(encoder_integration_test PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(encoder_integration_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME encoder_integration_test
        COMMAND encoder_integration_test
            ${CMAKE_CURRENT_BINARY_DIR}/encoder-integration-hevc.mp4
    )

    find_program(FFMPEG_EXECUTABLE NAMES ffmpeg)
    if(FFMPEG_EXECUTABLE)
        set(SEEK_TEST_VIDEO ${CMAKE_CURRENT_BINARY_DIR}/seek-test.mp4)
        add_test(NAME generate_seek_test_video
            COMMAND ${FFMPEG_EXECUTABLE}
                -hide_banner -loglevel error -y
                -f lavfi -i testsrc2=size=320x180:rate=30
                -t 3 -c:v libx264 -g 30 -pix_fmt yuv420p
                ${SEEK_TEST_VIDEO}
        )
        set_tests_properties(generate_seek_test_video PROPERTIES
            FIXTURES_SETUP seek_test_video
        )
        add_test(NAME decoder_seek_test
            COMMAND decoder_seek_test ${SEEK_TEST_VIDEO}
        )
        set_tests_properties(decoder_seek_test PROPERTIES
            FIXTURES_REQUIRED seek_test_video
        )
        if(APPLE)
            add_test(NAME decoder_videotoolbox_test
                COMMAND decoder_seek_test ${SEEK_TEST_VIDEO} videotoolbox
            )
            set_tests_properties(decoder_videotoolbox_test PROPERTIES
                FIXTURES_REQUIRED seek_test_video
                SKIP_RETURN_CODE 77
            )
        endif()
    endif()
endif()
