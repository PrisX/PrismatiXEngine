// A node .pxeffect for the raw drawing pixel test (GraphicsIntegrationTests):
// by `progress` it rotates the channels (red to green, green to blue, blue to
// red), then multiplies by the `tint` color in slot 0. Both steps are linear,
// so premultiplied input stays premultiplied.
cbuffer PrismatiXEffectContext : register(b0, space3) {
    float2 texelSize;
    float progress;
    float randomSeed;
    float4 parameters[8];
    float4 viewport;
};

Texture2D nodeTexture : register(t0, space2);
SamplerState nodeSampler : register(s0, space2);

struct Input {
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

float4 main(Input input) : SV_Target {
    float4 color = nodeTexture.Sample(nodeSampler, input.uv) * input.color;
    float4 rotated = float4(color.b, color.r, color.g, color.a);
    return lerp(color, rotated, saturate(progress)) * parameters[0];
}
