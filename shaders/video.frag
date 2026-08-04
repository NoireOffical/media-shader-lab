#version 150 core

#ifdef VIDEO_NV12
uniform sampler2DRect u_video_y;
uniform sampler2DRect u_video_uv;
uniform bool u_video_full_range;
uniform int u_yuv_matrix;
uniform vec2 u_video_size;
uniform vec2 u_chroma_size;
#else
uniform sampler2D u_video;
#endif
uniform int u_filter_mode;
uniform vec2 u_texel_size;
uniform float u_time;
uniform float u_effect_intensity;
uniform float u_edge_strength;
uniform float u_vignette_strength;
uniform bool u_split_view;

in vec2 v_tex_coord;
out vec4 out_color;

vec3 sample_video(vec2 coordinate) {
#ifdef VIDEO_NV12
    float y = texture(u_video_y, coordinate * u_video_size).r;
    vec2 chroma = texture(u_video_uv, coordinate * u_chroma_size).rg;
    if (u_video_full_range) {
        chroma -= vec2(0.5);
    } else {
        y = (y - 16.0 / 255.0) * (255.0 / 219.0);
        chroma = (chroma - vec2(128.0 / 255.0)) * (255.0 / 224.0);
    }

    float red;
    float green;
    float blue;
    if (u_yuv_matrix == 0) {
        red = y + 1.402 * chroma.y;
        green = y - 0.344136 * chroma.x - 0.714136 * chroma.y;
        blue = y + 1.772 * chroma.x;
    } else if (u_yuv_matrix == 2) {
        red = y + 1.4746 * chroma.y;
        green = y - 0.164553 * chroma.x - 0.571353 * chroma.y;
        blue = y + 1.8814 * chroma.x;
    } else {
        red = y + 1.5748 * chroma.y;
        green = y - 0.187324 * chroma.x - 0.468124 * chroma.y;
        blue = y + 1.8556 * chroma.x;
    }
    return clamp(vec3(red, green, blue), 0.0, 1.0);
#else
    return texture(u_video, coordinate).rgb;
#endif
}

vec3 edge_filter() {
    vec3 left = sample_video(v_tex_coord - vec2(u_texel_size.x, 0.0));
    vec3 right = sample_video(v_tex_coord + vec2(u_texel_size.x, 0.0));
    vec3 up = sample_video(v_tex_coord - vec2(0.0, u_texel_size.y));
    vec3 down = sample_video(v_tex_coord + vec2(0.0, u_texel_size.y));
    vec3 gradient = abs(right - left) + abs(down - up);
    return clamp(gradient * u_edge_strength, 0.0, 1.0);
}

void main() {
    vec3 original = sample_video(v_tex_coord);
    vec3 filtered = original;

    if (u_filter_mode == 1) {
        float luminance = dot(original, vec3(0.2126, 0.7152, 0.0722));
        filtered = vec3(luminance);
    } else if (u_filter_mode == 2) {
        filtered = vec3(
            dot(original, vec3(0.393, 0.769, 0.189)),
            dot(original, vec3(0.349, 0.686, 0.168)),
            dot(original, vec3(0.272, 0.534, 0.131))
        );
    } else if (u_filter_mode == 3) {
        filtered = edge_filter();
    } else if (u_filter_mode == 4) {
        vec2 centered = v_tex_coord - vec2(0.5);
        float radius = length(centered);
        float pulse = 0.01 * sin(u_time * 1.5);
        float mask = smoothstep(0.82 + pulse, 0.28, radius);
        filtered = original * mix(1.0, mask, u_vignette_strength);
    }

    vec3 color = mix(original, filtered, u_effect_intensity);
    if (u_split_view) {
        color = v_tex_coord.x < 0.5 ? original : color;
        if (abs(v_tex_coord.x - 0.5) < u_texel_size.x * 2.0) {
            color = vec3(1.0);
        }
    }
    out_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
