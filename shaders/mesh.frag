#version 450
// The material's page modulated by the shipped vertex colour, which is the only tie the archive gives between
// a surface and its colours (docs/formats/GEOMETRY.md §2's modulate bytes). With no named page the caller binds
// a white 1x1, so the untextured path stays byte-identical to the colour-only pass.

layout(binding = 0) uniform sampler2D page;   // wrapped: terrain UVs tile past one page

flat layout(location = 0) in vec4 vColour;
layout(location = 1) in vec2 vUv;
layout(location = 0) out vec4 fragColour;

vec3 toLinear(vec3 encoded) {
    return mix(encoded / 12.92, pow((encoded + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), encoded));
}

void main() {
    // Both sides arrive in linear space: the sRGB page formats decode on sample, and the attachment re-encodes
    // on write, so the modulate happens where the hardware says it should.
    fragColour = vec4(toLinear(vColour.rgb) * texture(page, vUv).rgb, 1.0);
}
