#version 450
// One triangle run straight out of a unit's general section (docs/formats/GEOMETRY.md §2): positions are signed
// i16 world units and colours are the shipped modulate bytes. The camera matrix comes in as push constants, so
// changing the view never rebuilds the vertex buffer.

layout(location = 0) in ivec3 aPosition;   // R16G16B16_SINT: raw world units, scaled by toClip
layout(location = 1) in vec4 aColour;      // R8G8B8A8_UNORM, the four bytes at +8..+11 of the record

layout(push_constant) uniform Push {
    mat4 toClip;   // column-major: centre, orbit, orthographic fit into [-1,1]
} push;

// Flat, not smooth: nothing in the archive says how these modulate bytes combine yet (no material ties one to a
// blend or interpolation rule), and a flat colour makes every pixel of a triangle equal to one shipped byte, which
// is what lets the offscreen probe assert exact coverage instead of "close enough".
flat layout(location = 0) out vec4 vColour;

void main() {
    // The shipped alpha byte is 0x80 for most vertices and marks nothing about coverage, so the first geometry
    // pass draws opaque. Transparency waits for a material that ties that byte to a blend mode.
    vColour = vec4(aColour.rgb, 1.0);
    gl_Position = push.toClip * vec4(vec3(aPosition), 1.0);
}
