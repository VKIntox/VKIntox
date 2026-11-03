#version 450

layout(set = 0, binding = 0) uniform sampler2D sceneImage;
layout(set = 0, binding = 1) uniform sampler2D validImage;

layout(location = 0) in vec2 textureCoord;
layout(location = 0) out vec4 outColor;

void main()
{
    if (texture(validImage, vec2(0.5)).r < 0.5)
        discard;

    outColor = vec4(texture(sceneImage, textureCoord).rgb, 1.0);
}
