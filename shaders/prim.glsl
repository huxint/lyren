// Shared by ui.vert and ui.frag; must match gfx::Prim.
#extension GL_EXT_buffer_reference : require

const uint KIND_RECT = 0u;
const uint KIND_SHADOW = 1u;
const uint KIND_GLYPH = 2u;
const uint KIND_IMAGE = 3u;
const uint KIND_ARC = 4u;
const uint KIND_WAVE = 5u;
const uint FLAG_GRADIENT = 1u;
const uint FLAG_CUTOUT = 2u;

struct Prim {
    vec4 rect;    // x, y, w, h in buffer pixels
    vec4 clip;    // x0, y0, x1, y1
    vec4 uv;      // u0, v0, u1, v1
    vec4 color;   // premultiplied
    vec4 color2;  // premultiplied border colour or gradient end
    vec4 radii;   // top-left, top-right, bottom-right, bottom-left
    vec4 params;  // rect: x border width; shadow: y sigma; arc: thickness, start, sweep; wave: thickness, amplitude, wavelength, phase
    uvec4 info;   // x kind, y texture, z flags
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Prims { Prim p[]; };
layout(push_constant) uniform PC {
    Prims prims;
    vec2 screen;
} pc;
