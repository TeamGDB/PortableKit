#version 450

// Level horizon: the game's picture turned about its centre and zoomed just
// enough to cover its rectangle, so no corner is ever left empty.

layout(set = 0, binding = 0) uniform sampler2D frame;

layout(push_constant) uniform Level {
    // Where the picture lies in the frame image: offset and size, 0 to 1.
    vec4 rect;
    // cos and sin of the turn (anticlockwise on screen), the zoom, and the
    // picture's width over its height.
    vec4 turn;
} level;

layout(location = 0) in vec2 picture;
layout(location = 0) out vec4 color;

void main() {
    // Centred, in units of the picture's height, y down as on screen.
    vec2 point = (picture - 0.5) * vec2(level.turn.w, 1.0);
    // The picture is turned anticlockwise as seen, which with y down is a
    // clockwise turn of the coordinates: sample where this point came from.
    float c = level.turn.x;
    float s = level.turn.y;
    vec2 source = vec2(c * point.x - s * point.y, s * point.x + c * point.y) / level.turn.z;
    vec2 uv = source / vec2(level.turn.w, 1.0) + 0.5;
    color = texture(frame, level.rect.xy + clamp(uv, 0.0, 1.0) * level.rect.zw);
}
