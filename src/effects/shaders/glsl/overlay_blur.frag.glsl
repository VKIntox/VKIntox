#version 450

// Separable Gaussian blur tap for the overlay backdrop. Runs on the
// half-resolution capture; each draw blurs along one axis. Multiple ping-pong
// draws (horizontal + vertical per "pass") compound into a wide, soft blur.
//
// One draw is a single axis so the shader stays a pure 1D convolution: the
// caller rotates the tap step for the vertical draw instead of branching here.

layout(set = 0, binding = 0) uniform sampler2D blurSource;

layout(location = 0) in vec2 textureCoord;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform BlurParams {
    vec2  direction;  // per-tap UV offset, already scaled by the blur radius
    float sigma;      // gaussian sigma expressed in tap units
    float padding;    // keeps the block a full 16 bytes for std430
} params;

const int kTaps = 9;  // symmetric, so 4 on each side plus the centre

void main()
{
    // Gaussian weights, normalised on the fly. sigma is a push constant so the
    // whole falloff is computed per-draw and costs nothing across a frame.
    vec4 sum = texture(blurSource, textureCoord);
    float weightSum = 1.0;

    for (int i = 1; i <= kTaps / 2; ++i)
    {
        float x = float(i);
        float w = exp(-0.5 * x * x / (params.sigma * params.sigma));
        weightSum += 2.0 * w;
        vec2 offset = params.direction * x;
        sum += (texture(blurSource, textureCoord + offset) +
                texture(blurSource, textureCoord - offset)) * w;
    }

    vec3 rgb = sum.rgb / weightSum;
    // opaque backdrop: the surface tint is applied by the window on top
    outColor = vec4(rgb, 1.0);
}
