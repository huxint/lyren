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
float gaussian(float x, float sigma) {
    return exp(-(x * x) / (2.0 * sigma * sigma)) / (2.5066283 * sigma);
}
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

// Arc of radius ra and half-thickness rb with round caps, symmetric around +y; sc = sin/cos of half the aperture.
float sd_arc(vec2 p, vec2 sc, float ra, float rb) {
    p.x = abs(p.x);
    return ((sc.y * p.x > sc.x * p.y) ? length(p - sc * ra) : abs(length(p) - ra)) - rb;
}

void main() {
    Prim p = pc.prims.p[v_index];
    if (v_pos.x < p.clip.x || v_pos.y < p.clip.y || v_pos.x >= p.clip.z || v_pos.y >= p.clip.w) {
        discard;
    }
    vec2 half_size = p.rect.zw * 0.5;
    vec2 local = v_pos - p.rect.xy - half_size;
    uint kind = p.info.x;
    if (kind == KIND_RECT) {
        float d = sd_rrect(local, half_size, p.radii);
        float cov = clamp(0.5 - d, 0.0, 1.0);
        if ((p.info.z & FLAG_GRADIENT) != 0u) {
            o = mix(p.color, p.color2, clamp(local.y / p.rect.w + 0.5, 0.0, 1.0)) * cov;
        } else if ((p.info.z & FLAG_HGRADIENT) != 0u) {
            o = mix(p.color, p.color2, clamp(local.x / p.rect.z + 0.5, 0.0, 1.0)) * cov;
        } else if (p.params.x > 0.0) {
            float inner = clamp(0.5 - d - p.params.x, 0.0, 1.0);
            o = p.color * inner + p.color2 * (cov - inner);
        } else {
            o = p.color * cov;
        }
    } else if (kind == KIND_SHADOW) {
        float a = rounded_box_shadow(local, half_size, p.params.y, p.radii.x);
        if ((p.info.z & FLAG_CUTOUT) != 0u) {
            a *= clamp(0.5 + sd_rrect(local, half_size, p.radii), 0.0, 1.0);
        }
        o = p.color * a;
    } else if (kind == KIND_GLYPH) {
        vec4 c = p.color;
        // A horizontal wipe: colour left of params.x, color2 right of it, blended over ±params.y.
        if ((p.info.z & FLAG_GRADIENT) != 0u) {
            c = mix(p.color, p.color2, smoothstep(p.params.x - p.params.y, p.params.x + p.params.y, v_pos.x));
        }
        o = c * texture(textures[nonuniformEXT(p.info.y)], v_uv).r;
    } else if (kind == KIND_ARC) {
        // Rotate so the middle of the arc points up (+y with y flipped to point up).
        float mid = p.params.y + p.params.z * 0.5, half_sweep = min(p.params.z * 0.5, 3.14159265);
        vec2 q = vec2(local.x, -local.y);
        q = vec2(cos(mid) * q.x - sin(mid) * q.y, sin(mid) * q.x + cos(mid) * q.y);
        float d = sd_arc(q, vec2(sin(half_sweep), cos(half_sweep)), min(half_size.x, half_size.y) - p.params.x * 0.5, p.params.x * 0.5);
        o = p.color * clamp(0.5 - d, 0.0, 1.0);
    } else if (kind == KIND_WAVE) {
        // A single stroke joins the played wave to the flat tail, including when paused.
        float th = p.params.x, amp = p.params.y, k = 6.2831853 / p.params.z;
        float x0 = p.rect.x + th * 0.5, x1 = p.rect.x + p.rect.z - th * 0.5;
        float at = p.rect.x + p.uv.x, end = clamp(at, x0, x1), span = end - x0;
        float x = clamp(v_pos.x, x0, x1), y = p.rect.y + half_size.y, slope = 0.0;
        amp *= smoothstep(0.0, 2.0 * p.params.z, span);
        if (amp > 0.0 && x < end) {
            float taper = min(p.params.z, span * 0.5);
            vec2 t = clamp(vec2(x - x0, end - x) / taper, 0.0, 1.0);
            // Quintic smootherstep has zero first and second derivatives at both ends.
            vec2 e = t * t * t * (t * (6.0 * t - 15.0) + 10.0);
            vec2 de = 30.0 * t * t * (t - 1.0) * (t - 1.0) / taper;
            float envelope = e.x * e.y, derivative = de.x * e.y - e.x * de.y;
            float s = (x - x0) * k + p.params.w;
            y -= amp * envelope * sin(s);
            slope = amp * (derivative * sin(s) + envelope * k * cos(s));
        }
        float d = x != v_pos.x ? length(v_pos - vec2(x, y)) : abs(v_pos.y - y) / sqrt(1.0 + slope * slope);
        float played = clamp(at - v_pos.x + 0.5, 0.0, 1.0);
        o = mix(p.color2, p.color, played) * clamp(0.5 - (d - th * 0.5), 0.0, 1.0);
    } else {
        float cov = clamp(0.5 - sd_rrect(local, half_size, p.radii), 0.0, 1.0);
        o = texture(textures[nonuniformEXT(p.info.y)], v_uv) * p.color * cov;
    }
}
