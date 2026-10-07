#version 450

// Level horizon (Angle + level horizon tilt controls): one triangle covering
// the picture's rectangle, which the viewport gives. The fragment shader
// turns and zooms the game's picture inside it.

layout(location = 0) out vec2 picture;

void main() {
    // (0,0), (2,0), (0,2): the triangle that covers the whole viewport.
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    picture = uv;
}
