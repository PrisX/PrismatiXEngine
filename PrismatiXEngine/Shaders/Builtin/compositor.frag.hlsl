cbuffer CompositorContext : register(b0, space3) {
    float2 texelSize;
    float blurAmount;
    float vignetteAmount;
    float colorGradeAmount;
    float randomSeed;
    float2 padding;
};

Texture2D stageTexture : register(t0, space2);
SamplerState stageSampler : register(s0, space2);

struct FragmentInput {
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

struct FragmentOutput {
    float4 color : SV_Target;
};

float4 sampleStage(float2 uv) {
    return stageTexture.Sample(stageSampler, clamp(uv, 0.0, 1.0));
}

FragmentOutput main(FragmentInput input) {
    const float radius = blurAmount * 5.0;
    const float2 offset = texelSize * radius;
    float4 stage = sampleStage(input.uv) * 0.28;
    stage += sampleStage(input.uv + float2(offset.x, 0.0)) * 0.12;
    stage += sampleStage(input.uv - float2(offset.x, 0.0)) * 0.12;
    stage += sampleStage(input.uv + float2(0.0, offset.y)) * 0.12;
    stage += sampleStage(input.uv - float2(0.0, offset.y)) * 0.12;
    stage += sampleStage(input.uv + offset) * 0.06;
    stage += sampleStage(input.uv - offset) * 0.06;
    stage += sampleStage(input.uv + float2(offset.x, -offset.y)) * 0.06;
    stage += sampleStage(input.uv + float2(-offset.x, offset.y)) * 0.06;

    const float3 graded = float3(
        dot(stage.rgb, float3(0.393, 0.769, 0.189)),
        dot(stage.rgb, float3(0.349, 0.686, 0.168)),
        dot(stage.rgb, float3(0.272, 0.534, 0.131)));
    stage.rgb = lerp(stage.rgb, saturate(graded), colorGradeAmount);

    const float2 centered = input.uv * 2.0 - 1.0;
    const float radial = dot(centered, centered);
    const float vignette = lerp(1.0, saturate(1.25 - radial * 0.75),
                                vignetteAmount);
    stage.rgb *= vignette;

    FragmentOutput output;
    output.color = stage * input.color;
    return output;
}
