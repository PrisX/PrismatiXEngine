cbuffer PrismatiXEffectContext : register(b0, space3) {
  float2 texelSize;
  float progress;
  float randomSeed;
  float4 parameters[8];
  float4 viewport; // logical x, y, width, height
};
Texture2D stageTexture : register(t0, space2);
SamplerState stageSampler : register(s0, space2);
struct Input { float4 color : COLOR0; float2 uv : TEXCOORD0; };
float4 main(Input input) : SV_Target {
  float4 color = stageTexture.Sample(stageSampler, input.uv) * input.color;
  float3 dream = float3(color.b, color.r, color.g);
  return float4(lerp(color.rgb, dream, saturate(parameters[0].x * progress)), color.a);
}
