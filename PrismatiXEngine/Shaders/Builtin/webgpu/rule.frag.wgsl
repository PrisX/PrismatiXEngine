struct type_u002e_RuleContext {
    targetSize: vec2<f32>,
    threshold: f32,
    vague: f32,
}

@group(3) @binding(0)
var<uniform> RuleContext: type_u002e_RuleContext;
@group(2) @binding(0)
var sourceTexture: texture_2d<f32>;
@group(2) @binding(1)
var sourceSampler: sampler;
@group(2) @binding(2)
var ruleTexture: texture_2d<f32>;
@group(2) @binding(3)
var ruleSampler: sampler;
var<private> in_u002e_var_u002e_COLOR0_1: vec4<f32>;
var<private> in_u002e_var_u002e_TEXCOORD0_1: vec2<f32>;
var<private> global: vec4<f32>;
var<private> out_u002e_var_u002e_SV_Target: vec4<f32>;

fn main_1() {
    let _e16 = in_u002e_var_u002e_COLOR0_1;
    let _e17 = in_u002e_var_u002e_TEXCOORD0_1;
    let _e18 = global;
    let _e21 = RuleContext.targetSize;
    let _e24 = textureSample(ruleTexture, ruleSampler, clamp((_e18.xy / _e21), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f)));
    let _e27 = RuleContext.vague;
    let _e30 = RuleContext.threshold;
    let _e34 = textureSample(sourceTexture, sourceSampler, _e17);
    let _e35 = (_e34 * _e16);
    out_u002e_var_u002e_SV_Target = vec4<f32>(_e35.x, _e35.y, _e35.z, (_e35.w * clamp((((_e24.x + _e27) - _e30) / _e27), 0f, 1f)));
    return;
}

@fragment
fn main(@location(0) in_u002e_var_u002e_COLOR0_: vec4<f32>, @location(1) in_u002e_var_u002e_TEXCOORD0_: vec2<f32>, @builtin(position) param: vec4<f32>) -> @location(0) vec4<f32> {
    in_u002e_var_u002e_COLOR0_1 = in_u002e_var_u002e_COLOR0_;
    in_u002e_var_u002e_TEXCOORD0_1 = in_u002e_var_u002e_TEXCOORD0_;
    global = param;
    main_1();
    let _e7 = out_u002e_var_u002e_SV_Target;
    return _e7;
}
