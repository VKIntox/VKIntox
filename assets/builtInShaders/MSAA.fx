/*
    MSAA.fx — from-scratch morphological anti-aliasing.

    This is a post-process (image-space) anti-aliasing filter in the MLAA
    family. It is not hardware multisampling: it looks for luminance
    discontinuities, measures how long each edge segment runs, and blends only
    the pixels near an edge's ends, where the staircase artifacts live. A
    straight, long edge gets no blend, so it stays crisp.

    Per the MLAA coverage model, a pixel at distance j from the nearer end of a
    segment of total length S contributes 0.5 * (1 - (2j + 1) / S), which is
    ~0.5 at the end pixel and falls to 0 through the middle.

    Defaults target a clean image at modest cost.
*/

#include "ReShade.fxh"

uniform float EdgeThreshold <
    ui_label = "Edge threshold";
    ui_type = "slider";
    ui_min = 0.02; ui_max = 0.50; ui_step = 0.01;
    ui_tooltip =
        "Minimum luminance difference that counts as an edge.\n"
        "Lower finds more (and softer) edges; higher stays conservative and\n"
        "leaves more aliasing. 0.10 is a good default for SDR.";
> = 0.10;

uniform int SearchSteps <
    ui_label = "Iterations (search steps)";
    ui_type = "slider";
    ui_min = 1; ui_max = 16; ui_step = 1;
    ui_tooltip =
        "How far along an edge the shader searches for its ends, in pixels.\n"
        "Longer segments get smoother corners, at a small per-pixel cost.\n"
        "8 covers most geometry; 12-16 helps long, shallow silhouettes.";
> = 8;

uniform float BlendStrength <
    ui_label = "Blend strength";
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_tooltip =
        "Scales every blend. 1.0 is full anti-aliasing; lower keeps a little\n"
        "of the original edge.";
> = 1.0;

float MSAALuma(float3 color)
{
    return dot(color, float3(0.299, 0.587, 0.114));
}

// Coverage of a pixel `near` pixels from one end of a segment `total` long.
float MSAACoverage(float near, float total)
{
    return saturate(0.5 * (1.0 - (2.0 * near + 1.0) / total));
}

// Measures how far the edge that sits `off` pixels away continues along `step`.
float2 MSAAMeasure(float2 texcoord, float2 pixel, float2 off, float2 step, int steps, float threshold)
{
    float left = 0.0;
    [loop]
    for (int i = 1; i <= steps; i++)
    {
        float2 a = texcoord - step * (float)i * pixel;
        float here = MSAALuma(tex2D(ReShade::BackBuffer, a).rgb);
        float there = MSAALuma(tex2D(ReShade::BackBuffer, a + off * pixel).rgb);
        if (abs(here - there) <= threshold)
            break;
        left = (float)i;
    }
    float right = 0.0;
    [loop]
    for (int j = 1; j <= steps; j++)
    {
        float2 a = texcoord + step * (float)j * pixel;
        float here = MSAALuma(tex2D(ReShade::BackBuffer, a).rgb);
        float there = MSAALuma(tex2D(ReShade::BackBuffer, a + off * pixel).rgb);
        if (abs(here - there) <= threshold)
            break;
        right = (float)j;
    }
    return float2(left, right);
}

float3 PS_MSAA(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    const float2 px = ReShade::PixelSize;
    float3 color = tex2D(ReShade::BackBuffer, texcoord).rgb;
    const float here = MSAALuma(color);

    // four edges, handled independently so a corner can pick up two blends
    [unroll]
    for (int e = 0; e < 4; e++)
    {
        const float2 off = (e == 0) ? float2(0.0, -1.0)
                         : (e == 1) ? float2(0.0,  1.0)
                         : (e == 2) ? float2(-1.0, 0.0)
                                    : float2( 1.0, 0.0);
        const float2 along = (e < 2) ? float2(1.0, 0.0) : float2(0.0, 1.0);

        const float neighbour = MSAALuma(tex2D(ReShade::BackBuffer, texcoord + off * px).rgb);
        if (abs(here - neighbour) <= EdgeThreshold)
            continue;

        const float2 measured = MSAAMeasure(texcoord, px, off, along, SearchSteps, EdgeThreshold);
        const float total = measured.x + measured.y + 1.0;
        const float weight = MSAACoverage(min(measured.x, measured.y), total) * BlendStrength;
        color = lerp(color, tex2D(ReShade::BackBuffer, texcoord + off * px).rgb, weight);
    }

    return color;
}

technique MSAA <
    ui_label = "MSAA (morphological)";
    ui_tooltip =
        "Post-process morphological anti-aliasing. Finds luminance edges and\n"
        "blends only the pixels at the ends of each edge segment, leaving long\n"
        "straight edges untouched. The search-step count is the iteration cap.";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_MSAA;
    }
}
