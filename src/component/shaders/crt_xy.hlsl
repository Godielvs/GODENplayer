cbuffer CrtConstants : register(b0)
{
    float2 renderSize;
    float2 outputSize;

    float2 inverseRenderSize;
    float exposure;
    float radiusScale;

    float decayFast;
    float decayTail;
    float coreSigma;
    float haloSigma;

    float coreWeight;
    float haloWeight;
    float beamIntensity;
    float gridOpacity;

    float3 phosphorColor;
    float inverseFastTau;

    float inverseTailTau;
    float midSideOrientation;
    float2 reserved;
};

struct FullscreenOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

FullscreenOutput FullscreenVS(uint vertexId : SV_VertexID)
{
    FullscreenOutput output;
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(
        uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = uv;
    return output;
}

Texture2D<float2> phosphorTexture : register(t0);
SamplerState linearSampler : register(s0);

float2 DecayPS(FullscreenOutput input) : SV_TARGET
{
    const float2 energy = phosphorTexture.SampleLevel(
        linearSampler, input.uv, 0.0);
    return energy * float2(decayFast, decayTail);
}

struct BeamInput
{
    float2 start : BEAM_START;
    float2 end : BEAM_END;
    float charge : BEAM_CHARGE;
    float age : BEAM_AGE;
    uint vertexId : SV_VertexID;
};

struct BeamOutput
{
    float4 position : SV_POSITION;
    nointerpolation float2 startPixel : TEXCOORD0;
    nointerpolation float2 endPixel : TEXCOORD1;
    nointerpolation float charge : TEXCOORD2;
    nointerpolation float age : TEXCOORD3;
};

BeamOutput BeamVS(BeamInput input)
{
    BeamOutput output;
    const float radius = min(renderSize.x, renderSize.y) * radiusScale;
    const float2 center = renderSize * 0.5;
    // Direct mode keeps the literal X=L/Y=R trace. Mid/Side rotates the same
    // samples into the conventional goniometer orientation, with mono on the
    // vertical axis. The orthonormal factor preserves the trace's apparent
    // energy and scale; this is a rotation only, not a second gain model.
    static const float inverseSqrtTwo = 0.7071067811865475;
    const float2 startMidSide = float2(
        (input.start.x - input.start.y) * inverseSqrtTwo,
        (input.start.x + input.start.y) * inverseSqrtTwo);
    const float2 endMidSide = float2(
        (input.end.x - input.end.y) * inverseSqrtTwo,
        (input.end.x + input.end.y) * inverseSqrtTwo);
    const float2 orientedStart = lerp(
        input.start, startMidSide, midSideOrientation);
    const float2 orientedEnd = lerp(
        input.end, endMidSide, midSideOrientation);
    const float2 startPixel = center +
        float2(orientedStart.x, -orientedStart.y) * radius;
    const float2 endPixel = center +
        float2(orientedEnd.x, -orientedEnd.y) * radius;
    const float2 delta = endPixel - startPixel;
    const float lengthValue = length(delta);
    const float2 tangent = lengthValue > 1.0e-5
        ? delta / lengthValue
        : float2(1.0, 0.0);
    const float2 normal = float2(-tangent.y, tangent.x);
    const float padding = max(3.75 * haloSigma, 2.0);
    const bool atEnd = input.vertexId >= 2;
    const bool positiveSide =
        input.vertexId == 1 || input.vertexId == 3;
    const float along = atEnd ? lengthValue + padding : -padding;
    const float across = positiveSide ? padding : -padding;
    const float2 pixel = startPixel + tangent * along + normal * across;
    output.position = float4(
        pixel * inverseRenderSize * float2(2.0, -2.0) +
            float2(-1.0, 1.0),
        0.0, 1.0);
    output.startPixel = startPixel;
    output.endPixel = endPixel;
    output.charge = input.charge;
    output.age = input.age;
    return output;
}

float erfApprox(float value)
{
    const float signValue = value < 0.0 ? -1.0 : 1.0;
    const float x = abs(value);
    const float t = 1.0 / (1.0 + 0.3275911 * x);
    const float polynomial =
        (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t -
            0.284496736) * t + 0.254829592) * t;
    return signValue * (1.0 - polynomial * exp(-x * x));
}

float gaussianSegment(
    float2 pixel,
    float2 startPixel,
    float2 endPixel,
    float sigma,
    float charge)
{
    static const float inverseSqrtTwoPi = 0.3989422804014327;
    static const float inverseTwoPi = 0.1591549430918953;
    static const float inverseSqrtTwo = 0.7071067811865475;
    const float2 delta = endPixel - startPixel;
    const float lengthValue = length(delta);
    if (lengthValue < 1.0e-3)
    {
        const float2 offset = pixel - startPixel;
        const float sigmaSquared = sigma * sigma;
        return charge * inverseTwoPi / sigmaSquared *
            exp(-0.5 * dot(offset, offset) / sigmaSquared);
    }

    const float2 tangent = delta / lengthValue;
    const float2 relative = pixel - startPixel;
    const float projection = dot(relative, tangent);
    const float perpendicular = relative.x * tangent.y -
        relative.y * tangent.x;
    const float longitudinal =
        0.5 * (
            erfApprox((lengthValue - projection) * inverseSqrtTwo / sigma) +
            erfApprox(projection * inverseSqrtTwo / sigma));
    const float transverse = inverseSqrtTwoPi / sigma *
        exp(-0.5 * perpendicular * perpendicular / (sigma * sigma));
    return charge / lengthValue * transverse * max(0.0, longitudinal);
}

float2 BeamPS(BeamOutput input) : SV_TARGET
{
    const float2 pixel = input.position.xy;
    const float core = gaussianSegment(
        pixel, input.startPixel, input.endPixel, coreSigma, input.charge);
    const float halo = gaussianSegment(
        pixel, input.startPixel, input.endPixel, haloSigma, input.charge);
    const float energy =
        (core * coreWeight + halo * haloWeight) * beamIntensity;
    const float age = max(0.0, input.age);
    return float2(
        energy * 0.92 * exp(-age * inverseFastTau),
        energy * 0.08 * exp(-age * inverseTailTau));
}

float4 CompositePS(FullscreenOutput input) : SV_TARGET
{
    const float2 energyLayers = phosphorTexture.SampleLevel(
        linearSampler, input.uv, 0.0);
    const float energy = max(0.0, energyLayers.x + energyLayers.y);
    const float luminance = 1.0 - exp(-energy * exposure);
    const float hotCore = smoothstep(0.52, 0.98, luminance);
    float3 color = lerp(
        phosphorColor,
        float3(0.94, 1.0, 0.98),
        hotCore * 0.72) * luminance;

    const float2 axisDistance = abs(input.uv - 0.5) * outputSize;
    const float axes = 1.0 -
        saturate(min(axisDistance.x, axisDistance.y) - 0.15);
    const float borderDistance = min(
        min(input.uv.x, 1.0 - input.uv.x) * outputSize.x,
        min(input.uv.y, 1.0 - input.uv.y) * outputSize.y);
    const float border = 1.0 - saturate(borderDistance - 0.25);
    const float grid = max(axes * 0.42, border) * gridOpacity;
    color += float3(0.16, 0.32, 0.28) * grid;
    color += float3(0.0028, 0.0045, 0.0042);
    color = saturate(color);
    const float3 encoded = lerp(
        color * 12.92,
        1.055 * pow(color, 1.0 / 2.4) - 0.055,
        step(0.0031308, color));
    return float4(encoded, 1.0);
}
