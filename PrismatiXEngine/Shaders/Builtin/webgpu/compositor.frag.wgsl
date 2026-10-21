struct type_u002e_CompositorContext {
    texelSize: vec2<f32>,
    blurAmount: f32,
    vignetteAmount: f32,
    colorGradeAmount: f32,
    randomSeed: f32,
    grayscaleAmount: f32,
    padding: f32,
}

@group(3) @binding(0)
var<uniform> CompositorContext: type_u002e_CompositorContext;
@group(2) @binding(0)
var stageTexture: texture_2d<f32>;
@group(2) @binding(1)
var stageSampler: sampler;
var<private> in_u002e_var_u002e_COLOR0_1: vec4<f32>;
var<private> in_u002e_var_u002e_TEXCOORD0_1: vec2<f32>;
var<private> out_u002e_var_u002e_SV_Target: vec4<f32>;

fn main_1() {
    let _e41 = in_u002e_var_u002e_COLOR0_1;
    let _e42 = in_u002e_var_u002e_TEXCOORD0_1;
    let _e44 = CompositorContext.blurAmount;
    let _e47 = CompositorContext.texelSize;
    let _e48 = (_e47 * (_e44 * 5f));
    let _e50 = textureSample(stageTexture, stageSampler, clamp(_e42, vec2<f32>(0f, 0f), vec2<f32>(1f, 1f)));
    let _e53 = vec2<f32>(_e48.x, 0f);
    let _e56 = textureSample(stageTexture, stageSampler, clamp((_e42 + _e53), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f)));
    let _e61 = textureSample(stageTexture, stageSampler, clamp((_e42 - _e53), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f)));
    let _e65 = vec2<f32>(0f, _e48.y);
    let _e68 = textureSample(stageTexture, stageSampler, clamp((_e42 + _e65), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f)));
    let _e73 = textureSample(stageTexture, stageSampler, clamp((_e42 - _e65), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f)));
    let _e78 = textureSample(stageTexture, stageSampler, clamp((_e42 + _e48), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f)));
    let _e83 = textureSample(stageTexture, stageSampler, clamp((_e42 - _e48), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f)));
    let _e90 = textureSample(stageTexture, stageSampler, clamp((_e42 + vec2<f32>(_e48.x, -(_e48.y))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f)));
    let _e97 = textureSample(stageTexture, stageSampler, clamp((_e42 + vec2<f32>(-(_e48.x), _e48.y)), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f)));
    let _e99 = (((((((((_e50 * 0.28f) + (_e56 * 0.12f)) + (_e61 * 0.12f)) + (_e68 * 0.12f)) + (_e73 * 0.12f)) + (_e78 * 0.06f)) + (_e83 * 0.06f)) + (_e90 * 0.06f)) + (_e97 * 0.06f));
    let _e100 = _e99.xyz;
    let _e107 = CompositorContext.colorGradeAmount;
    let _e110 = mix(_e100, clamp(vec3<f32>(dot(_e100, vec3<f32>(0.393f, 0.769f, 0.189f)), dot(_e100, vec3<f32>(0.349f, 0.686f, 0.168f)), dot(_e100, vec3<f32>(0.272f, 0.534f, 0.131f))), vec3<f32>(0f, 0f, 0f), vec3<f32>(1f, 1f, 1f)), vec3(_e107)).xyz;
    let _e114 = CompositorContext.grayscaleAmount;
    let _e118 = ((_e42 * 2f) - vec2<f32>(1f, 1f));
    let _e124 = CompositorContext.vignetteAmount;
    let _e127 = (mix(_e110, vec3(dot(_e110, vec3<f32>(0.299f, 0.587f, 0.114f))), vec3(_e114)).xyz * mix(1f, clamp((1.25f - (dot(_e118, _e118) * 0.75f)), 0f, 1f), _e124));
    out_u002e_var_u002e_SV_Target = (vec4<f32>(_e127.x, _e127.y, _e127.z, _e99.w) * _e41);
    return;
}

@fragment
fn main(@location(0) in_u002e_var_u002e_COLOR0_: vec4<f32>, @location(1) in_u002e_var_u002e_TEXCOORD0_: vec2<f32>) -> @location(0) vec4<f32> {
    in_u002e_var_u002e_COLOR0_1 = in_u002e_var_u002e_COLOR0_;
    in_u002e_var_u002e_TEXCOORD0_1 = in_u002e_var_u002e_TEXCOORD0_;
    main_1();
    let _e5 = out_u002e_var_u002e_SV_Target;
    return _e5;
}
