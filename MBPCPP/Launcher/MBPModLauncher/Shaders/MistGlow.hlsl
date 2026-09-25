cbuffer Constants : register(b0) {
    float4 params;      // x=intensity, y=screenWidth, z=screenHeight, w=time
    float4 shape0;      // x=centerX, y=centerY, z=halfWidth, w=halfHeight
    float4 shape1;      // x=roundness/radius, y=feather, z=shapeMode, w=reserved
    float4 glowColor;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};

sampler sampler0 : register(s0);
Texture2D texture0 : register(t0);

float hash21(float2 p) {
    p = frac(p * float2(127.1, 311.7));
    p += dot(p, p + 34.71);
    return frac(p.x * p.y);
}

float noise2(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);

    float a = hash21(i);
    float b = hash21(i + float2(1.0, 0.0));
    float c = hash21(i + float2(0.0, 1.0));
    float d = hash21(i + float2(1.0, 1.0));

    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float fbmMist(float2 p) {
    float value = 0.0;
    float amp = 0.55;

    [unroll]
    for (int i = 0; i < 4; ++i) {
        value += noise2(p) * amp;
        p = float2(
            p.x * 1.67 - p.y * 1.11,
            p.x * 1.11 + p.y * 1.67
        ) + 6.37;
        amp *= 0.48;
    }

    return value;
}

float sdRoundBox(float2 p, float2 b, float r) {
    float2 q = abs(p) - (b - r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float4 main(PS_INPUT input) : SV_Target {
    // The ImGui primitive is ONLY an alpha mask. Its RGB is deliberately ignored;
    // otherwise the white geometry survives into the output and the effect
    // degenerates into a plain white glow.
    float4 texCol = texture0.Sample(sampler0, input.uv);
    float alphaMask = texCol.a * input.col.a * glowColor.a;

    float intensity = max(params.x, 0.0);
    float time = params.w;

    float2 center = shape0.xy;
    float2 halfSize = max(shape0.zw, float2(1.0, 1.0));
    float roundness = max(shape1.x, 0.0);
    float feather = max(shape1.y, 1.0);
    float shapeMode = shape1.z; // 0 = rounded rect, 1 = circle, 2 = horizontal capsule

    float2 local = input.pos.xy - center;
    float2 norm = local / halfSize;

    float sdf = 0.0;
    float radial = 0.0;
    float2 flow = norm;

    if (shapeMode > 1.5) {
        // True capsule SDF. halfSize.x is the half-length of the center segment,
        // halfSize.y is the core radius. This follows a scrubber/slider stroke
        // rather than using any rectangular mask.
        float capsuleHalfLength = max(halfSize.x, 0.0);
        float capsuleRadius = max(halfSize.y, 1.0);

        float closestX = clamp(local.x, -capsuleHalfLength, capsuleHalfLength);
        float2 fromAxis = local - float2(closestX, 0.0);
        sdf = length(fromAxis) - capsuleRadius;

        float fullHalfWidth = max(capsuleHalfLength + capsuleRadius, 1.0);
        float longitudinal = abs(local.x) / fullHalfWidth;
        float transverse = abs(local.y) / capsuleRadius;
        radial = saturate(max(longitudinal * 0.46, transverse));

        flow = float2(
            local.x / fullHalfWidth,
            local.y / capsuleRadius);
    }
    else if (shapeMode > 0.5) {
        float radius = max(min(halfSize.x, halfSize.y), 1.0);
        sdf = length(local) - radius;
        radial = length(local) / radius;
        flow = local / radius;
    }
    else {
        float2 baseHalf = max(halfSize, float2(1.0 + roundness, 1.0 + roundness));
        sdf = sdRoundBox(local, baseHalf, min(roundness, min(baseHalf.x, baseHalf.y) - 1.0));
        radial = max(abs(norm.x), abs(norm.y));
        flow = norm;
    }

    // 1.0 on/inside the core, smoothly fading across the outer plume region.
    float plumeMask = 1.0 - smoothstep(0.0, feather, sdf);

    // Strongest emission near the center, with a secondary brighter band around
    // the silhouette so smoke feels like it is lifting off the control.
    float centerEmission = pow(saturate(1.0 - radial), 1.55);
    float edgeBand = exp(-abs(sdf) / max(feather * 0.34, 1.0));

    // Shape-local coordinates keep the motion bound to the actual control
    // instead of reading like a fullscreen field.
    float t = time * 0.22;
    float n1 = fbmMist(flow * 2.8 + float2(t * 0.72, -t * 0.28));
    float n2 = fbmMist(flow * 5.0 + float2(-t * 0.38, t * 0.56) + float2(n1 - 0.5, 0.5 - n1) * 1.28);
    float n3 = fbmMist(flow * 8.6 + float2(t * 0.18, t * 0.31) + float2(n2 - 0.5, n1 - 0.5) * 0.92);

    float cloud = smoothstep(0.30, 0.78, n1 * 0.58 + n2 * 0.42);
    float wisps = pow(saturate(1.0 - abs(n2 - n3) * 2.7), 4.8);
    float breakup = smoothstep(0.22, 0.76, n3);
    float breathe = 0.84 + 0.16 * sin(time * 1.1 + n1 * 6.28318);

    float density = saturate(cloud * 0.54 + wisps * 0.46);
    density *= saturate(centerEmission * 0.75 + edgeBand * 0.42 + 0.18);
    density *= lerp(0.58, 1.0, breakup) * breathe;
    density *= plumeMask;

    float3 violet = max(glowColor.rgb, float3(0.10, 0.03, 0.24));
    float3 coldBlue = float3(0.075, 0.25, 0.92);
    float3 warmCore = float3(0.28, 0.08, 0.42);

    float colorMix = saturate(n2 * 0.52 + wisps * 0.28);
    float3 smokeColor = lerp(violet, coldBlue, colorMix);
    smokeColor = lerp(smokeColor, warmCore, centerEmission * 0.22);
    smokeColor *= 0.30 + density * 1.34 + edgeBand * 0.16;

    float smokeAlpha = saturate(alphaMask * density * intensity * 1.55);
    return float4(smokeColor, smokeAlpha);
}