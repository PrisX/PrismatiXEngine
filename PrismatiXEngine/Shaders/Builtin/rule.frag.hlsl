// Rule (image-mask) background transition. The outgoing background is the
// drawn texture; the grayscale rule image is sampled in Stage-target space.
// Alpha matches the CPU path: saturate((rule + vague - threshold) / vague),
// with every value normalized to 0..1.
cbuffer RuleContext : register(b0, space3) {
    float2 targetSize;
    float threshold;
    float vague;
};

Texture2D sourceTexture : register(t0, space2);
SamplerState sourceSampler : register(s0, space2);
Texture2D ruleTexture : register(t1, space2);
SamplerState ruleSampler : register(s1, space2);

struct FragmentInput {
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float4 position : SV_Position;
};

struct FragmentOutput {
    float4 color : SV_Target;
};

FragmentOutput main(FragmentInput input) {
    const float2 ruleUv = clamp(input.position.xy / targetSize, 0.0, 1.0);
    const float rule = ruleTexture.Sample(ruleSampler, ruleUv).r;
    const float alpha = saturate((rule + vague - threshold) / vague);
    FragmentOutput output;
    output.color = sourceTexture.Sample(sourceSampler, input.uv) * input.color;
    output.color.a *= alpha;
    return output;
}
