#version 450

layout(set = 0, binding = 0) uniform sampler2D processedImage;
layout(set = 0, binding = 1) uniform sampler2D finalImage;
layout(set = 0, binding = 2) uniform sampler2D sceneImage;
layout(set = 0, binding = 3) uniform sampler2D validImage;

layout(location = 0) in vec2 textureCoord;
layout(location = 0) out vec4 outColor;

void main()
{
    vec3 processed = texture(processedImage, textureCoord).rgb;
    vec3 finalColor = texture(finalImage, textureCoord).rgb;
    vec3 sceneColor = texture(sceneImage, textureCoord).rgb;

    float valid = step(0.5, texture(validImage, vec2(0.5)).r);
    float blackFrame = step(1e-4, max(max(sceneColor.r, max(sceneColor.g, sceneColor.b)),
                                      max(finalColor.r, max(finalColor.g, finalColor.b))));

    vec3 delta = abs(finalColor - sceneColor);
    float maxDelta = max(max(delta.r, delta.g), delta.b);
    float mask = smoothstep(0.002, 0.02, maxDelta) * valid * blackFrame;

    outColor = vec4(mix(processed, finalColor, mask), 1.0);
}
