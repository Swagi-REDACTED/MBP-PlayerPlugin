cbuffer Constants : register(b0)
{
    float4 params; // x=time, y=width, z=height, w=unused
    float4 shape0; // selected card: centerX, centerY, halfWidth, halfHeight
    float4 shape1; // launch button: centerX, centerY, halfWidth, halfHeight
    float4 tint;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};

float sdRoundBox(float2 p, float2 b, float r)
{
    float2 q = abs(p) - (b - r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float halo(float2 pixel, float4 shape, float radius, float pulse)
{
    float d = sdRoundBox(pixel - shape.xy, max(shape.zw, 1.0), radius);
    float outside = exp(-max(d, 0.0) / 34.0);
    float inside = 1.0 - smoothstep(-14.0, 7.0, d);
    return saturate(outside * (0.28 + pulse * 0.12) + inside * 0.10);
}

float4 main(PS_INPUT input) : SV_Target
{
    const float t = params.x;
    const float pulseA = 0.5 + 0.5 * sin(t * 1.7);
    const float pulseB = 0.5 + 0.5 * sin(t * 2.1 + 1.4);

    float card = halo(input.pos.xy, shape0, 20.0, pulseA);
    float launch = halo(input.pos.xy, shape1, 17.0, pulseB);

    float strength = saturate(card * 0.54 + launch * 0.70);
    float3 coldEdge = float3(0.07, 0.22, 0.82);
    float3 color = lerp(coldEdge, tint.rgb, saturate(card + launch * 0.45));
    color *= 0.35 + strength * 0.95;

    return float4(color, strength * tint.a);
}
