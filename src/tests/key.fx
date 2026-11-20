// KeyUniform test shader. Deliberately dumb: the output color IS the uniform
// value, so a wrong value can never hide behind other effect logic.
//
//   channel        meaning
//   R channel      hold uniform: 1.0 while the key is down
//   G channel      hold uniform inverted: 1.0 while the key is up
//   B channel      1 when R and G agree exactly (sanity against partial writes)
//
// R+G should never both be bright at once; a frame where neither R nor G is
// saturated means the uniform was not written at all.

uniform bool keyHold <
    source = "key";
    keycode = 0x77; // VK_F8, harmless on every layout
>;

uniform bool keyHoldInverted <
    source = "key";
    keycode = 0x77;
    invert = true;
>;

void keyColorVS(out float4 pos : SV_Position, out float2 uv : TEXCOORD, uint vid : SV_VertexID)
{
    // fullscreen triangle
    float x = float((vid << 1) & 2);
    float y = float(vid & 2);
    uv = float2(x, y);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 keyColorPS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float r = keyHold ? 1.0 : 0.0;
    float g = keyHoldInverted ? 1.0 : 0.0;
    float consistent = (keyHold == !keyHoldInverted) ? 1.0 : 0.0;
    return float4(r, g, consistent, 1.0);
}

technique KeyTest
{
    pass
    {
        VertexShader = keyColorVS;
        PixelShader = keyColorPS;
    }
}
