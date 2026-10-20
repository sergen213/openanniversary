#version 450
// Samples one mip-mapped texture and outputs premultiplied colour, which is what the pipeline's blend state
// expects: BC1/BC3 carry their own alpha, and tag-0x15 masks are white with their shape in alpha
// (docs/formats/TEXTURES.md §3).

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColour;

void main() {
    const vec4 texel = texture(uTexture, vTexCoord);
    fragColour = vec4(texel.rgb * texel.a, texel.a);
}
