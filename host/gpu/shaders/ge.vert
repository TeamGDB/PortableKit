#version 450

// PSP geometry. Transformed vertices arrive in object space and are multiplied
// by the combined world-view-projection matrix; "through" vertices are already
// in screen pixels and are mapped to clip space with the viewport size.
layout(location = 0) in vec4 in_position;
layout(location = 1) in vec2 in_texcoord;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 frag_texcoord;
layout(location = 1) out vec4 frag_color;

layout(push_constant) uniform Push {
    mat4 transform;      // WVP, or identity for through vertices
    vec4 viewport;       // xy: target size in PSP pixels, zw: unused
    vec4 texture_params; // x: texture enabled, y: texture function, z: alpha ref, w: alpha func
    vec4 uv_transform;   // xy: scale, zw: offset
} push;

void main() {
    frag_texcoord = in_texcoord * push.uv_transform.xy + push.uv_transform.zw;
    frag_color = in_color;
    if (push.viewport.z > 0.5) {
        // Screen-space vertices: pixels to clip space.
        vec2 ndc = vec2(in_position.x / push.viewport.x, in_position.y / push.viewport.y) * 2.0 - 1.0;
        gl_Position = vec4(ndc, clamp(in_position.z / 65535.0, 0.0, 1.0), 1.0);
    } else {
        vec4 clip = push.transform * vec4(in_position.xyz, 1.0);
        // PSP clip space follows OpenGL with z in [-w, w]; Vulkan clips against
        // [0, w], so without this remap the near half of every frustum is lost.
        // The PSP viewport's z scale and offset are folded into the Vulkan
        // viewport's min/max depth, which expects this [0, 1] device z.
        clip.z = (clip.z + clip.w) * 0.5;
        gl_Position = clip;
    }
}
