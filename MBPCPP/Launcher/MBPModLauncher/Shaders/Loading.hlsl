cbuffer Constants : register(b0) {
    float4 params;   // x=time, y=width, z=height, w=opacity
    float4 shape0;
    float4 shape1;
    float4 tint;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};

float hash21(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float valueNoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);

    float a = hash21(i);
    float b = hash21(i + float2(1.0, 0.0));
    float c = hash21(i + float2(0.0, 1.0));
    float d = hash21(i + float2(1.0, 1.0));

    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float fbm(float2 p) {
    float v = 0.0;
    float a = 0.5;

    [unroll]
    for (int i = 0; i < 4; ++i) {
        v += valueNoise(p) * a;
        p = float2(
            p.x * 1.62 - p.y * 1.18,
            p.x * 1.18 + p.y * 1.62
        ) + 9.13;
        a *= 0.5;
    }

    return v;
}

float4 main(PS_INPUT input) : SV_Target {
    float time = params.x;
    float2 resolution = max(params.yz, float2(1.0, 1.0));
    float opacity = saturate(params.w);

    float2 uv = input.pos.xy / resolution;
    float2 p = uv - 0.5;
    p.x *= resolution.x / resolution.y;

    // Slow motion keeps the loading state feeling calm rather than busy.
    float t = time * 0.16;

    float n1 = fbm(p * 2.15 + float2(t * 0.75, -t * 0.42));
    float n2 = fbm(p * 3.05 + float2(-t * 0.48, t * 0.68) + n1 * 1.35);
    float n3 = fbm(p * 4.30 + float2(t * 0.21, t * 0.31) + n2 * 0.95);

    float2 warp = float2(n1 - 0.5, n2 - 0.5);
    float2 q = p + warp * 0.30;

    float ribbonA = exp(-abs(q.y + sin(q.x * 2.2 + t * 2.7) * 0.11 + (n2 - 0.5) * 0.25) * 7.0);
    float ribbonB = exp(-abs(q.y - 0.18 + cos(q.x * 2.8 - t * 2.0) * 0.13 - (n1 - 0.5) * 0.22) * 8.5);
    float ribbonC = exp(-abs(q.y + 0.23 + sin(q.x * 3.6 + t * 1.3) * 0.08 + (n3 - 0.5) * 0.18) * 10.0);

    float3 topColor = float3(0.014, 0.018, 0.036);
    float3 bottomColor = float3(0.004, 0.006, 0.014);
    float3 color = lerp(bottomColor, topColor, saturate(1.0 - uv.y));

    // Violet is dominant, with a restrained cold-blue edge so it matches the
    // existing purple/blue player accents without turning neon.
    color += float3(0.24, 0.075, 0.62) * ribbonA * (0.34 + n3 * 0.40);
    color += float3(0.035, 0.24, 0.62) * ribbonB * (0.22 + n1 * 0.30);
    color += float3(0.33, 0.07, 0.45) * ribbonC * (0.16 + n2 * 0.22);

    float core = 1.0 - smoothstep(0.05, 0.72, length(p * float2(0.82, 1.0)));
    color += float3(0.08, 0.035, 0.20) * core * 0.72;

    // Fine moving highlights inside the fluid field.
    float filament = pow(saturate(1.0 - abs(n2 - n1) * 3.2), 7.0);
    color += float3(0.12, 0.14, 0.34) * filament * 0.20;

    // Vignette and tiny temporal grain avoid flat gradients/banding.
    float vignette = 1.0 - smoothstep(0.36, 0.98, length(p * float2(0.76, 1.0)));
    color *= 0.52 + vignette * 0.48;

    float grain = hash21(input.pos.xy + floor(time * 37.0)) - 0.5;
    color += grain * 0.010;

    color = max(color, 0.0);
    return float4(color, opacity);
}