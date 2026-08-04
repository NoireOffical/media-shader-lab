#version 150 core

uniform sampler2D u_video;
uniform int u_filter_mode;
uniform vec2 u_texel_size;
uniform float u_time;
uniform float u_effect_intensity;
uniform float u_edge_strength;
uniform float u_vignette_strength;
uniform bool u_split_view;

in vec2 v_tex_coord;
out vec4 out_color;

vec3 edge_filter() {
    vec3 left = texture(u_video, v_tex_coord - vec2(u_texel_size.x, 0.0)).rgb;
    vec3 right = texture(u_video, v_tex_coord + vec2(u_texel_size.x, 0.0)).rgb;
    vec3 up = texture(u_video, v_tex_coord - vec2(0.0, u_texel_size.y)).rgb;
    vec3 down = texture(u_video, v_tex_coord + vec2(0.0, u_texel_size.y)).rgb;
    vec3 gradient = abs(right - left) + abs(down - up);
    return clamp(gradient * u_edge_strength, 0.0, 1.0);
}

void main() {
    vec3 original = texture(u_video, v_tex_coord).rgb;
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
