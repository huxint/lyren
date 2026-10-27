#version 460
#extension GL_GOOGLE_include_directive : require
#include "prim.glsl"

layout(location = 0) out vec2 v_pos;
layout(location = 1) out vec2 v_uv;
layout(location = 2) flat out uint v_index;

void main() {
    Prim p = pc.prims.p[gl_InstanceIndex];
    vec2 c = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    vec4 r = p.rect;
    if (p.info.x == KIND_SHADOW) {
        r += vec4(-3.0, -3.0, 6.0, 6.0) * p.params.y;
    }
    vec2 pos = r.xy + c * r.zw;
    v_pos = pos;
    v_uv = mix(p.uv.xy, p.uv.zw, c);
    v_index = gl_InstanceIndex;
    gl_Position = vec4(pos / pc.screen * 2.0 - 1.0, 0.0, 1.0);
}
