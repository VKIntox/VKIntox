/*
    ExponentialFog.fx — exponential height fog.

    Linearizes the depth buffer with ReShade's depth input settings so it is
    correct across APIs, including Vulkan, then fades a coloured fog in with
    distance and screen height. Pixels at the far plane are treated as skybox
    and left alone.

    Needs ReShade 4.4+ and a detected depth buffer.
*/

#include "ReShade.fxh"

// Corrects the depth lookup UV for the depth input settings.
float2 FogDepthUv(float2 uv)
{
#if RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN
    uv.y = 1.0 - uv.y;
#endif
    uv.x /= RESHADE_DEPTH_INPUT_X_SCALE;
    uv.y /= RESHADE_DEPTH_INPUT_Y_SCALE;
#if RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET
    uv.x -= RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET * BUFFER_RCP_WIDTH;
#else
    uv.x -= RESHADE_DEPTH_INPUT_X_OFFSET / 2.000000001;
#endif
#if RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET
    uv.y += RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET * BUFFER_RCP_HEIGHT;
#else
    uv.y += RESHADE_DEPTH_INPUT_Y_OFFSET / 2.000000001;
#endif
    return uv;
}

// Linearizes a raw depth value to 0..1 depth.
float FogLinearize(float depth)
{
    depth *= RESHADE_DEPTH_MULTIPLIER;

#if RESHADE_DEPTH_INPUT_IS_LOGARITHMIC
    const float C = 0.01;
    depth = (exp(depth * log(C + 1.0)) - 1.0) / C;
#endif
#if RESHADE_DEPTH_INPUT_IS_REVERSED
    depth = 1.0 - depth;
#endif

    const float N = 1.0;
    depth /= RESHADE_DEPTH_LINEARIZATION_FAR_PLANE - depth * (RESHADE_DEPTH_LINEARIZATION_FAR_PLANE - N);
    return saturate(depth);
}

// Linear depth for a pixel, 0 at the camera and 1 at the far plane.
float FogLinearDepth(float2 texcoord)
{
    const float raw = tex2Dlod(ReShade::DepthBuffer, float4(FogDepthUv(texcoord), 0, 0)).x;
    return FogLinearize(raw);
}

uniform float3 FogColor <
    ui_label = "Fog color";
    ui_type = "color";
    ui_tooltip = "The color the fog blends toward.";
> = float3(0.70, 0.75, 0.80);

uniform float FogDensity <
    ui_label = "Fog density";
    ui_type = "slider";
    ui_min = 0.0; ui_max = 100.0; ui_step = 0.1;
    ui_tooltip =
        "How quickly the fog thickens with distance.\n"
        "10 is light, 30-50 is a heavy wall of fog.";
> = 10.0;

uniform float FogStart <
    ui_label = "Fog start";
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.001;
    ui_tooltip = "Distance where the fog begins, 0 at the camera and 1 at the far plane.";
> = 0.0;

uniform float FogHeight <
    ui_label = "Fog height";
    ui_type = "slider";
    ui_min = -1.0; ui_max = 1.0; ui_step = 0.01;
    ui_tooltip = "Vertical screen position of the fog layer, -1 at the bottom and 1 at the top.";
> = 0.0;

uniform float FogFalloff <
    ui_label = "Height falloff";
    ui_type = "slider";
    ui_min = 0.001; ui_max = 10.0; ui_step = 0.01;
    ui_tooltip = "How sharply the fog fades away from the fog height.";
> = 2.0;

uniform float FogOpacity <
    ui_label = "Maximum opacity";
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.001;
    ui_tooltip = "Upper bound on the blend strength, so distant fog never fully hides the scene.";
> = 1.0;

uniform float SkyThreshold <
    ui_label = "Sky depth threshold";
    ui_type = "slider";
    ui_min = 0.9000; ui_max = 1.0000; ui_step = 0.0001;
    ui_tooltip = "Linear depth above this counts as skybox and is left unfogged.";
> = 0.9999;

float3 PS_ExponentialFog(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    const float3 color = tex2D(ReShade::BackBuffer, texcoord).rgb;
    const float depth = FogLinearDepth(texcoord);

    // the skybox sits at the far plane, so it must not be fogged
    if (depth >= SkyThreshold || depth >= 1.0)
        return color;

    const float distanceFog = 1.0 - exp(-FogDensity * max(depth - FogStart, 0.0));

    // texcoord.y is 0 at the top and 1 at the bottom; remap to 1..-1 so a
    // positive FogHeight lifts the fog up the screen
    const float vertical = 1.0 - texcoord.y * 2.0;
    const float heightFog = exp(-abs(vertical - FogHeight) * FogFalloff);

    const float fog = saturate(distanceFog * heightFog * FogOpacity);
    return lerp(color, FogColor, fog);
}

technique ExponentialFog <
    ui_label = "Exponential height fog";
    ui_tooltip =
        "Distance and height based fog that leaves the skybox alone.\n"
        "Linearizes the depth buffer with ReShade's depth input settings, so it\n"
        "across APIs including Vulkan. Needs a detected depth buffer.";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_ExponentialFog;
    }
}
