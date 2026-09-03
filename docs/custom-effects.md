# Custom render effects

PrismatiX custom effects extend the existing native Stage compositor. They are
offline assets, not runtime shader injection: the Packager validates a bounded
`.pxeffect` descriptor, compiles its HLSL fragment entry point to SPIR-V, DXIL,
and MSL with SDL ShaderCross, reflects the resource layout, fingerprints each
artifact, and removes the HLSL source from the Player archive.

## Descriptor

```json
{
  "format": "PrismatiXEffect",
  "schemaRevision": 2,
  "id": "dream-tone",
  "targetLayer": "stage",
  "shader": "Effects/dream-tone.frag.hlsl",
  "uniforms": [
    {
      "name": "amount",
      "type": "number",
      "slot": 0,
      "default": 0.5,
      "minimum": 0,
      "maximum": 1
    }
  ]
}
```

An effect may declare at most eight uniquely named uniforms in unique slots.
Supported types are `number`, `vec2`, and `color`; defaults must be finite and
inside the declared range. Colors also remain in normalized `[0, 1]` space.

## Fixed HLSL interface

```hlsl
cbuffer PrismatiXEffectContext : register(b0, space3) {
  float2 texelSize;
  float progress;
  float randomSeed;
  float4 parameters[8];
};
Texture2D stageTexture : register(t0, space2);
SamplerState stageSampler : register(s0, space2);
```

The fragment entry point is `main`. Reflection must report exactly one sampler,
one uniform buffer, one output, and no storage textures or buffers. The runtime
provides only the Stage texture, normalized progress, logical-viewport texel
size, deterministic seed, and declared parameter slots. Raw renderer objects,
GPU pointers, filesystem, network, wall-clock, and unrestricted randomness are
never exposed.

## JavaScript

```ts
ctx.stage.customEffect("dream-tone", 0.65, {amount: 0.8});
ctx.stage.clearCustomEffect();
```

Named values are checked against the packaged schema before native uniform
updates. The positional `vec4[]` form remains available for compatibility with
older extensions. Per-frame progress sampling and rendering stay native; Stage
effect state, parameters, and deterministic seed participate in save, seek, and
rollback capture.

## Capability and fallback

Custom effects require `graphicsTier: "gpu-effects"`. Native Player and Native
Preview load the artifact format accepted by the SDL GPU device and reject the
package if no compatible or fingerprint-valid artifact is available. The WASM
Preview currently has no custom GPU-effect backend and fails the GPU tier
explicitly instead of silently rendering a different image. Use basic-tier
built-in effects where a software/WASM fallback is required.

