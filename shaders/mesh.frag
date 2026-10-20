#version 450
// Vertex colours only; no material lookup yet (docs/formats/GEOMETRY.md §6). The colour bytes are already
// display-encoded, and the attachment is an sRGB format that encodes on write, so the shader hands over linear
// values and lets the hardware put them back: what reads back out is the shipped byte again.

flat layout(location = 0) in vec4 vColour;
layout(location = 0) out vec4 fragColour;

vec3 toLinear(vec3 encoded) {
    return mix(encoded / 12.92, pow((encoded + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), encoded));
}

void main() { fragColour = vec4(toLinear(vColour.rgb), 1.0); }
