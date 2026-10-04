/*
    CAS.fx — from-scratch Contrast Adaptive Sharpening.

    A local-contrast sharpener in the spirit of AMD FidelityFX CAS. The
    sharpen amount is derived per channel from how close the 3x3 neighborhood
    is to the signal limits: flat regions get little boost, high-contrast
    edges get more, and clipped extremes get none, so it does not ring or
    oversharpen.

    Defaults are tuned for a clean 1080p/1440p image without visible halos.
*/

#include "ReShade.fxh"

uniform float Sharpness <
    ui_label = "Sharpness";
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_tooltip =
        "Adaptive sharpening amount.\n"
        "0.0 = off, 0.7 = balanced default, 1.0 = maximum.\n"
        "The filter backs off automatically on bright/dark extremes, so it\n"
        "does not clip or halo the way a plain unsharp mask does.";
> = 0.7;

uniform int Iterations <
    ui_label = "Iterations";
    ui_type = "slider";
    ui_min = 1; ui_max = 3; ui_step = 1;
    ui_tooltip =
        "How many times the sharpen is applied.\n"
        "1 = natural, 2-3 = progressively crisper for soft or heavily\n"
        "reconstructed sources. Re-applying compounds the contrast boost.";
> = 1;

uniform float ClampHighlights <
    ui_label = "Protect highlights";
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_tooltip =
        "Fades sharpening out of near-white pixels to avoid sparkling on\n"
        "speculars and UI. 0.0 = no protection, 1.0 = strong.";
> = 0.5;

float3 CaseSharp(float3 a, float3 b, float3 c,
                 float3 d, float3 e, float3 f,
                 float3 g, float3 h, float3 i,
                 float sharpness)
{
    // Soft min/max of the full 3x3, in AMD's doubled convention so the
    // 2.0 signal bound matches.
    float3 mn = min(min(d, e), min(f, min(b, h)));
    float3 mx = max(max(d, e), max(f, max(b, h)));
    mn += min(mn, min(min(a, c), min(g, i)));
    mx += max(mx, max(max(a, c), max(g, i)));

    // Amplify by local headroom: no room left at either extreme -> no sharpening.
    float3 amplify = saturate(min(mn, 2.0 - mx) / max(mx, 1.0e-5));
    amplify = sqrt(amplify);

    // Filter shape: 0 w 0 / w 1 w / 0 w 0. peak goes -8..-5 with sharpness.
    const float peak = -(8.0 - 3.0 * saturate(sharpness));
    float3 w = amplify / peak;

    float3 sharpened = ((b + d + f + h) * w + e) / (1.0 + 4.0 * w);
    return saturate(sharpened);
}

float3 PS_CAS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    const float2 px = ReShade::PixelSize;

    float3 result = tex2D(ReShade::BackBuffer, texcoord).rgb;

    [loop]
    for (int iteration = 0; iteration < Iterations; iteration++)
    {
        float3 a = tex2D(ReShade::BackBuffer, texcoord + float2(-1, -1) * px).rgb;
        float3 b = tex2D(ReShade::BackBuffer, texcoord + float2( 0, -1) * px).rgb;
        float3 c = tex2D(ReShade::BackBuffer, texcoord + float2( 1, -1) * px).rgb;
        float3 d = tex2D(ReShade::BackBuffer, texcoord + float2(-1,  0) * px).rgb;
        float3 f = tex2D(ReShade::BackBuffer, texcoord + float2( 1,  0) * px).rgb;
        float3 g = tex2D(ReShade::BackBuffer, texcoord + float2(-1,  1) * px).rgb;
        float3 h = tex2D(ReShade::BackBuffer, texcoord + float2( 0,  1) * px).rgb;
        float3 i = tex2D(ReShade::BackBuffer, texcoord + float2( 1,  1) * px).rgb;

        result = CaseSharp(a, b, c, d, result, f, g, h, i, Sharpness);
    }

    // Fade the sharpen off near white: the boost on `result` minus the source.
    const float source_luma = dot(tex2D(ReShade::BackBuffer, texcoord).rgb, float3(0.299, 0.587, 0.114));
    const float protect = 1.0 - ClampHighlights * smoothstep(0.8, 1.0, source_luma);
    return lerp(tex2D(ReShade::BackBuffer, texcoord).rgb, result, protect);
}

technique CAS <
    ui_label = "CAS";
    ui_tooltip =
        "Contrast Adaptive Sharpening. Sharpens detail locally without\n"
        "haloing: flat areas stay flat, hard edges get crisper, and near-white\n"
        "speculars are protected.";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_CAS;
    }
}
