// Active DX12 shader, extracted unchanged from main.cpp.
#ifndef NUM_GRADIENT_STOPS
#define NUM_GRADIENT_STOPS 4
#endif

#ifndef SMOKE_OCTAVES
#define SMOKE_OCTAVES 6
#endif

#ifndef BORDER_MODE
#define BORDER_MODE 0
#endif

cbuffer SmokeParams : register(b0)
{
    float  _Time;                  // elapsed seconds
    float2 _Resolution;            // viewport pixels
    float  _Intensity;             // overall brightness      [0–1]  default 1.0
    float  _Speed;                 // animation speed mult    [0–5]  default 1.0
    float  _NoiseScale;            // UV scale for noise      [0–10] default 2.5
    float  _WarpStrength;          // domain warp amount      [0–8]  default 4.0
    float  _TendrilAmount;         // directional wisps       [0–1]  default 0.4
    float  _ColorRotationSpeed;    // palette spin speed      [0–1]  default 0.08
    float  _BorderWidth;           // border mask thickness   [0–0.5] default 0.12

    float4 _Color0;   // default: Google Blue   (0.259, 0.522, 0.957, 1)
    float4 _Color1;   // default: Purple         (0.608, 0.447, 0.796, 1)
    float4 _Color2;   // default: Coral/Pink     (0.851, 0.396, 0.439, 1)
    float4 _Color3;   // default: Light Blue     (0.482, 0.667, 0.969, 1)
    float4 _Color4;   // default: unused         (1, 1, 1, 1)
    float4 _Color5;   // default: unused         (1, 1, 1, 1)
    float4 _Color6;   // default: unused         (1, 1, 1, 1)
    float4 _Color7;   // default: unused         (1, 1, 1, 1)
};

// ═════════════════════════════════════════════════════════════════════════════
// Default Initialiser (call once if you aren't binding a cbuffer)
// ═════════════════════════════════════════════════════════════════════════════

void InitSmokeDefaults(
    inout float  intensity,
    inout float  speed,
    inout float  noiseScale,
    inout float  warpStrength,
    inout float  tendrilAmount,
    inout float  colorRotSpeed,
    inout float  borderWidth,
    inout float4 colors[8])
{
    intensity     = 1.0;
    speed         = 1.0;
    noiseScale    = 2.5;
    warpStrength  = 4.0;
    tendrilAmount = 0.4;
    colorRotSpeed = 0.08;
    borderWidth   = 0.12;

    colors[0] = float4(0.259, 0.522, 0.957, 1.0); // Google Blue
    colors[1] = float4(0.608, 0.447, 0.796, 1.0); // Purple
    colors[2] = float4(0.851, 0.396, 0.439, 1.0); // Coral/Pink
    colors[3] = float4(0.482, 0.667, 0.969, 1.0); // Light Blue
    colors[4] = float4(1.0, 1.0, 1.0, 1.0);
    colors[5] = float4(1.0, 1.0, 1.0, 1.0);
    colors[6] = float4(1.0, 1.0, 1.0, 1.0);
    colors[7] = float4(1.0, 1.0, 1.0, 1.0);
}

// ── Helpers ──

float3 Mod289_3(float3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
float2 Mod289_2(float2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
float3 Permute(float3 x)  { return Mod289_3(((x * 34.0) + 1.0) * x); }

// ── Simplex Noise 2D ──
// Ashima Arts / Ian McEwan — hash-based, no texture lookups.

float SimplexNoise2D(float2 v)
{
    const float4 C = float4(
        0.211324865405187,  // (3 - sqrt(3)) / 6
        0.366025403784439,  // (sqrt(3) - 1) / 2
        -0.577350269189626,  // -1 + 2 * C.x
        0.024390243902439   // 1 / 41
    );

    // First corner
    float2 i  = floor(v + dot(v, C.yy));
    float2 x0 = v - i + dot(i, C.xx);

    // Other corners
    float2 i1 = (x0.x > x0.y) ? float2(1.0, 0.0) : float2(0.0, 1.0);
    float4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;

    // Permutations
    i = Mod289_2(i);
    float3 p = Permute(Permute(i.y + float3(0.0, i1.y, 1.0))
                            + i.x + float3(0.0, i1.x, 1.0));

    float3 m = max(0.5 - float3(
        dot(x0,     x0),
        dot(x12.xy, x12.xy),
        dot(x12.zw, x12.zw)
    ), 0.0);

    m = m * m;
    m = m * m;

    // Gradients
    float3 x  = 2.0 * frac(p * C.www) - 1.0;
    float3 h  = abs(x) - 0.5;
    float3 ox = floor(x + 0.5);
    float3 a0 = x - ox;

    m *= 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);

    float3 g;
    g.x  = a0.x * x0.x     + h.x * x0.y;
    g.yz = a0.yz * x12.xz  + h.yz * x12.yw;

    return 130.0 * dot(m, g);
}

// ── Fractal Brownian Motion ─────────────────────────────────────────────────

float FBM(float2 p)
{
    float value     = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;

    [unroll]
    for (int i = 0; i < SMOKE_OCTAVES; i++)
    {
        value     += amplitude * SimplexNoise2D(p * frequency);
        frequency *= 2.0;
        amplitude *= 0.5;
    }

    return value;
}

// ── Domain-Warped Smoke ─────────────────────────────────────────────────────
// Two layers of FBM warp for organic cloud flow.
// Technique: Inigo Quilez — https://iquilezles.org/articles/warp/

float WarpedSmoke(float2 uv, float t, float warpStr)
{
    float2 q = float2(
        FBM(uv + float2(0.0, 0.0) + 0.15 * t),
        FBM(uv + float2(5.2, 1.3) + 0.12 * t)
    );

    float2 r = float2(
        FBM(uv + warpStr * q + float2(1.7, 9.2) + 0.126 * t),
        FBM(uv + warpStr * q + float2(8.3, 2.8) + 0.130 * t)
    );

    return FBM(uv + warpStr * r);
}

// Samples an N-stop looping gradient at position `t` ∈ [0, 1].
// The stops are evenly spaced and the gradient wraps around.

float3 SampleGradient(float t, float4 colors[8], int numStops)
{
    t = frac(t); // ensure [0, 1)
    float  segment   = t * (float)numStops;
    int    idx       = (int)floor(segment) % numStops;
    int    nextIdx   = (idx + 1) % numStops;
    float  localT    = frac(segment);

    return lerp(colors[idx].rgb, colors[nextIdx].rgb, localT);
}

float ComputeBorderMask(float2 uv, float borderWidth)
{
    float dx = min(uv.x, 1.0 - uv.x);
    float dy = min(uv.y, 1.0 - uv.y);
    float borderDist = min(dx, dy);

    float mask = 1.0 - smoothstep(0.0, borderWidth, borderDist);

    // Corner rounding
    float2 cornerUv   = abs(uv - 0.5) * 2.0;
    float  cornerDist = length(max(cornerUv - float2(0.75, 0.75), 0.0));
    float  cornerMask = smoothstep(0.0, 0.2, cornerDist);

    return max(mask, cornerMask * 0.3);
}


// uv    - normalised screen / mesh UVs in [0, 1]
// time  - elapsed seconds (e.g. _Time.y in Unity)
// resolution  - viewport size in pixels
//
// All other params come from the cbuffer / material properties.

float4 GeminiSmoke(
    float2 uv,
    float  time,
    float2 resolution,
    float  intensity,
    float  speed,
    float  noiseScale,
    float  warpStrength,
    float  tendrilAmount,
    float  colorRotSpeed,
    float  borderWidth,
    float4 gradientColors[8],
    int    numGradientStops)
{
    float aspect = resolution.x / resolution.y;
    float2 st   = uv;
    st.x *= aspect;

    float t = time * speed;

    // Smoke noise
    float smoke1 = WarpedSmoke(st * noiseScale,        t * 0.8,       warpStrength);
    float smoke2 = WarpedSmoke(st * noiseScale * 1.4 + 10.0, t * 0.6 + 5.0, warpStrength);
    float smoke  = lerp(smoke1, smoke2, 0.5) * 0.5 + 0.5;

    float tendril = SimplexNoise2D(float2(
        uv.x * 8.0 + t * 0.3,
        uv.y * 3.0 - t * 0.5
    ));
    smoke += pow(abs(tendril), 2.0) * tendrilAmount;

#if BORDER_MODE == 1
    smoke *= ComputeBorderMask(uv, borderWidth);
#endif
    float angle    = atan2(uv.y - 0.5, uv.x - 0.5);
    float hueShift = angle / 6.28318530718 + t * colorRotSpeed;

    float3 color = SampleGradient(hueShift, gradientColors, numGradientStops);

    // Highlight bloom on dense areas
    color += 0.15 * pow(smoke, 3.0);

    float alpha = saturate(smoke * intensity * 0.6);

    return float4(color, alpha);
}


// ═════════════════════════════════════════════════════════════════════════════
// CONVENIENCE WRAPPER — uses the cbuffer directly
// ═════════════════════════════════════════════════════════════════════════════

float4 GeminiSmoke_Auto(float2 uv)
{
    float4 colors[8] = {
        _Color0, _Color1, _Color2, _Color3,
        _Color4, _Color5, _Color6, _Color7
    };

    return GeminiSmoke(
        uv,
        _Time,
        _Resolution,
        _Intensity,
        _Speed,
        _NoiseScale,
        _WarpStrength,
        _TendrilAmount,
        _ColorRotationSpeed,
        _BorderWidth,
        colors,
        NUM_GRADIENT_STOPS
    );
}


// ═════════════════════════════════════════════════════════════════════════════
// QUICK-START WRAPPER — zero config, just pass uv + time
// ═════════════════════════════════════════════════════════════════════════════
//
//   float4 col = GeminiSmoke_Simple(uv, _Time.y, _ScreenParams.xy);
//

float4 GeminiSmoke_Simple(float2 uv, float time, float2 resolution)
{
    float4 defaultColors[8] = {
        float4(0.259, 0.522, 0.957, 1.0),  // Google Blue
        float4(0.608, 0.447, 0.796, 1.0),  // Purple
        float4(0.851, 0.396, 0.439, 1.0),  // Coral / Pink
        float4(0.482, 0.667, 0.969, 1.0),  // Light Blue
        float4(1, 1, 1, 1),
        float4(1, 1, 1, 1),
        float4(1, 1, 1, 1),
        float4(1, 1, 1, 1)
    };

    return GeminiSmoke(
        uv,
        time,
        resolution,
        /* intensity       */ 1.0,
        /* speed           */ 1.0,
        /* noiseScale      */ 2.5,
        /* warpStrength    */ 4.0,
        /* tendrilAmount   */ 0.4,
        /* colorRotSpeed   */ 0.08,
        /* borderWidth     */ 0.12,
        defaultColors,
        4  // NUM_GRADIENT_STOPS
    );
}


cbuffer SmokeConstants : register(b1)
{
    float4x4 ProjectionMatrix;
    float4 RectBounds; // minX, minY, maxX, maxY
    float4 _SmokeColors[8];
    float4 _SmokeParams;  // x=_Time, y=_Intensity, z=_Speed, w=_NoiseScale
    float4 _SmokeParams2; // x=_WarpStrength, y=_TendrilAmount, z=_ColorRotationSpeed, w=_BorderWidth
    float4 _SmokeParams3; // x=_Rounding
};

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float2 screenPos : TEXCOORD0;
};

VS_OUTPUT VSMain(uint vertexId : SV_VertexID)
{
    VS_OUTPUT output;
    float2 bMin = RectBounds.xy - float2(25.0, 25.0);
    float2 bMax = RectBounds.zw + float2(25.0, 25.0);
    float2 pos[6] = {
        float2(bMin.x, bMin.y),
        float2(bMax.x, bMin.y),
        float2(bMin.x, bMax.y),
        float2(bMin.x, bMax.y),
        float2(bMax.x, bMin.y),
        float2(bMax.x, bMax.y)
    };
    float2 p = pos[vertexId];
    output.pos = mul(ProjectionMatrix, float4(p, 0.0, 1.0));
    output.screenPos = p;
    return output;
}

float sdRoundRectSmoke(float2 p, float2 b, float r)
{
    float2 d = abs(p) - b + float2(r, r);
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;
}

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float2 bMin = RectBounds.xy;
    float2 bMax = RectBounds.zw;
    float2 size = (bMax - bMin) * 0.5;
    float2 center = bMin + size;

    float rounding = _SmokeParams3.x;
    float dist = sdRoundRectSmoke(input.screenPos - center, size, rounding);
    if (dist > 0.5) discard;

    float2 uv = (input.screenPos - bMin) / (bMax - bMin);

    float4 colors[8] = {
        _SmokeColors[0], _SmokeColors[1], _SmokeColors[2], _SmokeColors[3],
        _SmokeColors[4], _SmokeColors[5], _SmokeColors[6], _SmokeColors[7]
    };

    float4 smokeColor = GeminiSmoke(
        uv,
        _SmokeParams.x,
        bMax - bMin,
        _SmokeParams.y,
        _SmokeParams.z,
        _SmokeParams.w,
        _SmokeParams2.x,
        _SmokeParams2.y,
        _SmokeParams2.z,
        _SmokeParams2.w,
        colors,
        4
    );

    // Antialiased edge
    smokeColor.a *= 1.0 - smoothstep(-0.5, 0.5, dist);
    return smokeColor;
}
