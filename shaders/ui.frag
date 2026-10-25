#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "prim.glsl"

layout(set = 0, binding = 0) uniform sampler2D textures[];

layout(location = 0) in vec2 v_pos;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in uint v_index;
layout(location = 0) out vec4 o;

// Signed distance to a rounded rectangle centred at the origin (y down).
float sd_rrect(vec2 p, vec2 half_size, vec4 r) {
    float rad = p.x < 0.0 ? (p.y < 0.0 ? r.x : r.w) : (p.y < 0.0 ? r.y : r.z);
    vec2 q = abs(p) - half_size + rad;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - rad;
}

// Gaussian-blurred rounded box, after Evan Wallace's "fast rounded rectangle shadows".
float gaussian(float x, float sigma) { return exp(-(x * x) / (2.0 * sigma * sigma)) / (2.5066283 * sigma); }
vec2 erf(vec2 x) {
    vec2 s = sign(x), a = abs(x);
    x = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    x *= x;
    return s - s / (x * x);
}
float shadow_x(float x, float y, float sigma, float corner, vec2 half_size) {
    float delta = min(half_size.y - corner - abs(y), 0.0);
    float curved = half_size.x - corner + sqrt(max(0.0, corner * corner - delta * delta));
    vec2 integral = 0.5 + 0.5 * erf((x + vec2(-curved, curved)) * (0.70710678 / sigma));
    return integral.y - integral.x;
}
float rounded_box_shadow(vec2 p, vec2 half_size, float sigma, float corner) {
    float low = p.y - half_size.y, high = p.y + half_size.y;
    float start = clamp(-3.0 * sigma, low, high), end = clamp(3.0 * sigma, low, high);
    float step = (end - start) / 4.0, y = start + step * 0.5, value = 0.0;
    for (int i = 0; i < 4; i++) {
        value += shadow_x(p.x, p.y - y, sigma, corner, half_size) * gaussian(y, sigma) * step;
        y += step;
    }
    return value;
}

void main() {
    Prim p = pc.prims.p[v_index];
    if (v_pos.x < p.clip.x || v_pos.y < p.clip.y || v_pos.x >= p.clip.z || v_pos.y >= p.clip.w) discard;
    vec2 half_size = p.rect.zw * 0.5;
    vec2 local = v_pos - p.rect.xy - half_size;
    uint kind = p.info.x;
    if (kind == KIND_RECT) {
        float d = sd_rrect(local, half_size, p.radii);
        float cov = clamp(0.5 - d, 0.0, 1.0);
        if ((p.info.z & FLAG_GRADIENT) != 0u) {
            o = mix(p.color, p.color2, clamp(local.y / p.rect.w + 0.5, 0.0, 1.0)) * cov;
        } else if (p.params.x > 0.0) {
            float inner = clamp(0.5 - d - p.params.x, 0.0, 1.0);
            o = p.color * inner + p.color2 * (cov - inner);
        } else {
            o = p.color * cov;
        }
    } else if (kind == KIND_SHADOW) {
        float a = rounded_box_shadow(local, half_size, p.params.y, p.radii.x);
        if ((p.info.z & FLAG_CUTOUT) != 0u) a *= clamp(0.5 + sd_rrect(local, half_size, p.radii), 0.0, 1.0);
        o = p.color * a;
    } else if (kind == KIND_GLYPH) {
        o = p.color * texture(textures[nonuniformEXT(p.info.y)], v_uv).r;
    } else {
        float cov = clamp(0.5 - sd_rrect(local, half_size, p.radii), 0.0, 1.0);
        o = texture(textures[nonuniformEXT(p.info.y)], v_uv) * p.color * cov;
    }
}
