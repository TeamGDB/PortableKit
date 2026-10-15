#version 450

// Antialiasing (video.antialiasing = fxaa): fast approximate antialiasing of
// the finished picture, at the internal resolution, before it is scaled to the
// window. The vertex shader is the level horizon's: one triangle over the
// viewport, which is the whole picture.
//
// For each pixel: if the contrast around it is low, it is left as it is.
// Otherwise the edge through it is found (horizontal or vertical, from the
// second differences of luma), followed both ways along the picture until the
// luma no longer matches the edge, and the pixel is resampled moved towards
// the other side of the edge by how near it lies to the closer end, so a
// staircase becomes a slope. Thin features shorter than a pixel get a blend
// with their neighbours instead.

layout(set = 0, binding = 0) uniform sampler2D frame;

layout(push_constant) uniform Pass {
    // The size of one texel of the picture, 0 to 1.
    vec2 texel;
} pass;

layout(location = 0) in vec2 picture;
layout(location = 0) out vec4 color;

// Contrast below which a pixel is no edge: relative to the brightest
// neighbour, and in dark areas an absolute floor.
const float kContrast = 0.125;
const float kContrastFloor = 0.0312;
// How much of the blend thin features take, 0 to 1.
const float kSubpixel = 0.75;
// Steps along an edge, in texels: fine near the pixel, longer further away.
const int kSteps = 10;
const float kStep[kSteps] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 4.0, 8.0);

float luma(vec3 rgb) { return dot(rgb, vec3(0.299, 0.587, 0.114)); }
float luma_at(vec2 uv) { return luma(textureLod(frame, uv, 0.0).rgb); }
float luma_near(vec2 uv, vec2 offset) { return luma_at(uv + offset * pass.texel); }

void main() {
    vec2 uv = picture;
    vec4 centre = textureLod(frame, uv, 0.0);
    float m = luma(centre.rgb);
    float n = luma_near(uv, vec2(0, -1));
    float s = luma_near(uv, vec2(0, 1));
    float e = luma_near(uv, vec2(1, 0));
    float w = luma_near(uv, vec2(-1, 0));
    float low = min(m, min(min(n, s), min(e, w)));
    float high = max(m, max(max(n, s), max(e, w)));
    float range = high - low;
    if (range < max(kContrastFloor, high * kContrast)) {
        color = centre;
        return;
    }
    float nw = luma_near(uv, vec2(-1, -1));
    float ne = luma_near(uv, vec2(1, -1));
    float sw = luma_near(uv, vec2(-1, 1));
    float se = luma_near(uv, vec2(1, 1));

    // Thin features: how far the pixel stands out from the average around it.
    float average = (2.0 * (n + s + e + w) + nw + ne + sw + se) / 12.0;
    float subpixel = smoothstep(0.0, 1.0, clamp(abs(average - m) / range, 0.0, 1.0));
    subpixel = subpixel * subpixel * kSubpixel;

    // Luma changing from row to row is a horizontal edge; from column to
    // column, a vertical one.
    float across_rows = abs(nw + sw - 2.0 * w) + 2.0 * abs(n + s - 2.0 * m) + abs(ne + se - 2.0 * e);
    float across_columns = abs(nw + ne - 2.0 * n) + 2.0 * abs(w + e - 2.0 * m) + abs(sw + se - 2.0 * s);
    bool horizontal = across_rows >= across_columns;

    // Which side of the pixel the edge lies on: where the luma changes most.
    float before = horizontal ? n : w;
    float after = horizontal ? s : e;
    float change_before = abs(before - m);
    float change_after = abs(after - m);
    bool towards_before = change_before >= change_after;
    float gradient = max(change_before, change_after);
    float across = horizontal ? pass.texel.y : pass.texel.x;
    float edge_luma;
    if (towards_before) {
        across = -across;
        edge_luma = 0.5 * (before + m);
    } else {
        edge_luma = 0.5 * (after + m);
    }

    // Follow the edge, half a texel across, both ways until either end.
    vec2 on_edge = uv;
    if (horizontal) on_edge.y += 0.5 * across;
    else on_edge.x += 0.5 * across;
    vec2 along = horizontal ? vec2(pass.texel.x, 0.0) : vec2(0.0, pass.texel.y);
    float threshold = 0.25 * gradient;
    vec2 forward = on_edge + along * kStep[0];
    vec2 backward = on_edge - along * kStep[0];
    float forward_change = luma_at(forward) - edge_luma;
    float backward_change = luma_at(backward) - edge_luma;
    bool forward_done = abs(forward_change) >= threshold;
    bool backward_done = abs(backward_change) >= threshold;
    for (int i = 1; i < kSteps && !(forward_done && backward_done); ++i) {
        if (!forward_done) {
            forward += along * kStep[i];
            forward_change = luma_at(forward) - edge_luma;
            forward_done = abs(forward_change) >= threshold;
        }
        if (!backward_done) {
            backward -= along * kStep[i];
            backward_change = luma_at(backward) - edge_luma;
            backward_done = abs(backward_change) >= threshold;
        }
    }
    float forward_distance = horizontal ? forward.x - uv.x : forward.y - uv.y;
    float backward_distance = horizontal ? uv.x - backward.x : uv.y - backward.y;
    bool forward_nearer = forward_distance <= backward_distance;
    float distance = min(forward_distance, backward_distance);
    float span = forward_distance + backward_distance;
    // Moved only when the nearer end crosses the edge the other way from
    // this pixel: that is where the staircase steps.
    float end_change = forward_nearer ? forward_change : backward_change;
    bool pixel_darker = m < edge_luma;
    float edge_offset = ((end_change < 0.0) != pixel_darker) ? 0.5 - distance / span : 0.0;
    float offset = max(edge_offset, subpixel);

    vec2 moved = uv;
    if (horizontal) moved.y += offset * across;
    else moved.x += offset * across;
    color = vec4(textureLod(frame, moved, 0.0).rgb, centre.a);
}
