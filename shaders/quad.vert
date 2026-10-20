#version 450
// One textured quad in clip space. `scale` letterboxes the image so its aspect ratio survives a window resize
// without touching the vertex buffer (docs/ARCHITECTURE.md "Conventions": no abstraction before it is needed).

layout(location = 1) in vec2 aTexCoord;   // v = 0 is the image's first row, which clip y = -1 maps to the top
layout(location = 0) in vec2 aPosition;   // already in [-1,1] clip space

layout(push_constant) uniform Push {
    vec2 scale;
} push;

layout(location = 0) out vec2 vTexCoord;

void main() {
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPosition * push.scale, 0.0, 1.0);
}
