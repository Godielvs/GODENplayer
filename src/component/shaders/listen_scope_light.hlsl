// GODENplayer Listen Scope optical pass.
// Spatial-only: every mask is rebuilt from the current presented 512-sample
// curve. No texture from an earlier visual revision is sampled.

struct MaskInput {
    float2 position : POSITION;
    float2 coverage : TEXCOORD0;
};

struct MaskOutput {
    float4 position : SV_POSITION;
    float2 coverage : TEXCOORD0;
};

MaskOutput MaskVS(MaskInput input)
{
    MaskOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.coverage = saturate(input.coverage);
    return output;
}

float2 MaskPS(MaskOutput input) : SV_TARGET
{
    return input.coverage;
}

struct FullscreenOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

FullscreenOutput FullscreenVS(uint vertexId : SV_VertexID)
{
    FullscreenOutput output;
    const float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(
        corner.x * 2.0 - 1.0,
        1.0 - corner.y * 2.0,
        0.0,
        1.0);
    output.uv = corner;
    return output;
}

Texture2D<float2> sourceMask : register(t0);
SamplerState linearClamp : register(s0);

cbuffer BlurParameters : register(b0) {
    float2 blurDeltaUv;
    float2 blurPadding;
};

float2 BlurPS(FullscreenOutput input) : SV_TARGET
{
    float2 value = sourceMask.SampleLevel(linearClamp, input.uv, 0) *
        0.2270270270;
    value += (sourceMask.SampleLevel(
            linearClamp, input.uv - blurDeltaUv, 0) +
        sourceMask.SampleLevel(
            linearClamp, input.uv + blurDeltaUv, 0)) * 0.1945945946;
    value += (sourceMask.SampleLevel(
            linearClamp, input.uv - 2.0 * blurDeltaUv, 0) +
        sourceMask.SampleLevel(
            linearClamp, input.uv + 2.0 * blurDeltaUv, 0)) * 0.1216216216;
    value += (sourceMask.SampleLevel(
            linearClamp, input.uv - 3.0 * blurDeltaUv, 0) +
        sourceMask.SampleLevel(
            linearClamp, input.uv + 3.0 * blurDeltaUv, 0)) * 0.0540540541;
    value += (sourceMask.SampleLevel(
            linearClamp, input.uv - 4.0 * blurDeltaUv, 0) +
        sourceMask.SampleLevel(
            linearClamp, input.uv + 4.0 * blurDeltaUv, 0)) * 0.0162162162;
    return value;
}

Texture2D<float2> currentMask : register(t0);
Texture2D<float2> nearMask : register(t1);
Texture2D<float2> wideMask : register(t2);

cbuffer CompositeParameters : register(b0) {
    float3 accentLinear;
    float opticalStrength;
};

float4 CompositePS(FullscreenOutput input) : SV_TARGET
{
    const float2 mask = currentMask.SampleLevel(linearClamp, input.uv, 0);
    const float2 nearValue = nearMask.SampleLevel(linearClamp, input.uv, 0);
    const float2 wideValue = wideMask.SampleLevel(linearClamp, input.uv, 0);
    const float core = saturate(mask.x);
    const float envelope = saturate(mask.y);
    const float nearCoverage = saturate(nearValue.x + 0.35 * nearValue.y);
    const float wideCoverage = saturate(wideValue.x + 0.35 * wideValue.y);
    const float density = saturate(1.7 * nearCoverage + 0.6 * wideCoverage);
    const float3 tint = lerp(
        saturate(accentLinear), 1.0.xxx, 0.18 + 0.55 * density);
    const float coreEnergy = core * (1.05 + 0.35 * density) +
        0.12 * envelope;
    const float haloEnergy = 0.42 * nearCoverage + 0.14 * wideCoverage;
    const float3 radiance = tint * coreEnergy +
        saturate(accentLinear) * haloEnergy;
    const float3 mapped = 1.0 - exp(-max(0.0.xxx, radiance));
    // The sRGB RTV performs the one output transfer. RGB-only screen blending
    // composes this bounded emission over the already-rendered D2D plot.
    float3 output = saturate(mapped * abs(opticalStrength));
    if (opticalStrength < 0.0) {
        output = lerp(12.92 * output,
            1.055 * pow(max(output, 0.0.xxx), 1.0 / 2.4) - 0.055,
            step(0.0031308.xxx, output));
    }
    return float4(saturate(output), 0.0);
}
