// ============================================================================

// DX12 integration contract (not automatically registered with ShaderLoader):
// Compile VSMain as vs_5_0 and each PS_* entry below as ps_5_0.
// VSMain uses DrawInstanced(6, 1, 0, 0), triangle-list, no vertex buffer.
// Bind b0 to BOTH shader stages, an SRV table t0..t6, and a linear-clamp s0.
// FluidUniforms is 144 bytes / 36 root constants. If using a CBV, allocate
// and align its view to 256 bytes. Respect HLSL's 16-byte packing below.
// Use floating-point simulation targets (e.g. R16G16B16A16_FLOAT), no blending,
// no depth/culling. Ping-pong every read/write target; transition RTV <-> SRV.
// Clear simulation textures to zero initially; dye alpha stores coverage.
// Set texelSize=1/source-resolution, dt in seconds, aspectRatio=width/height,
// mouse positions in normalized top-left UVs, positive gradient biases,
// dissipation in [0,1], and splatOpacity=1 for dye (0 is fine for velocity).
// PS_Composite is opaque and expects an UNORM (not SRGB) output target because
// it performs background gamma conversion itself. Composite before ImGui.
// Shopify Editions Spring 2026 - Developer Portion Fluid Shader (HLSL Port)
// Translated 1:1 from the original WebGL / React Three Fiber implementation.
// ============================================================================

// --- COMMON UNIFORMS & SAMPLERS ---
SamplerState linearClampSampler : register(s0);

// Textures used across various passes
Texture2D<float4> uVelocity   : register(t0);
Texture2D<float4> uSource     : register(t1);
Texture2D<float4> uPressure   : register(t2);
Texture2D<float4> uDivergence : register(t3);
Texture2D<float4> uCurl       : register(t4);
Texture2D<float4> uTarget     : register(t5); // For splatting
Texture2D<float4> tMap        : register(t6); // Final composite fluid dye

cbuffer FluidUniforms : register(b0) {
    float2 texelSize;
    float dt;
    float dissipation;
    float curl;
    
    // Splat (Mouse/Pointer tracking)
    // Feeds both velocity and dye via two separate passes (one writing to uVelocity, one to tMap)
    float2 pointPos;
    float splatOpacity; // byte 28: fills existing padding; dye coverage [0,1]
    float2 prevPointPos;   
    float3 splatColor;     
    float splatRadius;     
    float aspectRatio;
    
    // Background / Composite settings (Developer gray gradient)
    float uBackgroundMode;
    float uBackgroundPointCount;
    float uBackgroundBias1;
    float uBackgroundBias2;
    float uBackgroundAngle;
    float3 uBackgroundColor1;
    float3 uBackgroundColor2;
    float3 uBackgroundColor3;
    float uBackgroundDarken;
};

// --- VERTEX SHADER ---
struct appdata {
    float4 vertex : POSITION;
    float2 uv : TEXCOORD0;
};

struct v2f {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float2 vL : TEXCOORD1;
    float2 vR : TEXCOORD2;
    float2 vT : TEXCOORD3;
    float2 vB : TEXCOORD4;
};

v2f VS_Quad(appdata v) {
    v2f o;
    o.pos = float4(v.vertex.xy, 0.0, 1.0);
    o.uv = v.uv;
    
    // Precalculate neighbor UVs for finite difference operations
    o.vL = o.uv - float2(texelSize.x, 0.0);
    o.vR = o.uv + float2(texelSize.x, 0.0);
    o.vB = o.uv - float2(0.0, texelSize.y);
    o.vT = o.uv + float2(0.0, texelSize.y);
    
    return o;
}

// Procedural full-screen quad matching the app's existing six-vertex draws.
// VS_Quad remains available for callers supplying POSITION/TEXCOORD vertices.
v2f VSMain(uint vertexId : SV_VertexID) {
    const float2 uv[6] = {
        float2(0, 0), float2(1, 0), float2(0, 1),
        float2(0, 1), float2(1, 0), float2(1, 1)
    };
    appdata v;
    v.uv = uv[vertexId];
    v.vertex = float4(v.uv.x * 2.0 - 1.0, 1.0 - v.uv.y * 2.0, 0.0, 1.0);
    return VS_Quad(v);
}

// ============================================================================
// FLUID SIMULATION PASSES
// ============================================================================

// --- PASS 1: ADVECTION ---
// Moves velocity and dye through the vector field
float4 PS_Advect(v2f i) : SV_Target {
    float2 coord = i.uv - dt * uVelocity.Sample(linearClampSampler, i.uv).xy * texelSize;
    float4 result = dissipation * uSource.Sample(linearClampSampler, coord);
    // Preserve advected dye coverage; forcing alpha to one hides the background.
    return result;
}

// --- PASS 2: CURL / VORTICITY ---
// Calculates the curl of the velocity field
float4 PS_Curl(v2f i) : SV_Target {
    float L = uVelocity.Sample(linearClampSampler, i.vL).y;
    float R = uVelocity.Sample(linearClampSampler, i.vR).y;
    float B = uVelocity.Sample(linearClampSampler, i.vB).x;
    float T = uVelocity.Sample(linearClampSampler, i.vT).x;
    
    float vorticity = R - L - T + B;
    return float4(0.5 * vorticity, 0.0, 0.0, 1.0);
}

// --- PASS 3: VORTICITY CONFINEMENT ---
// Re-injects energy lost during advection
float4 PS_VorticityConfinement(v2f i) : SV_Target {
    float L = uCurl.Sample(linearClampSampler, i.vL).x;
    float R = uCurl.Sample(linearClampSampler, i.vR).x;
    float B = uCurl.Sample(linearClampSampler, i.vB).x;
    float T = uCurl.Sample(linearClampSampler, i.vT).x;
    float C = uCurl.Sample(linearClampSampler, i.uv).x;
    
    float2 force = 0.5 * float2(abs(T) - abs(B), abs(R) - abs(L));
    force /= length(force) + 0.0001;
    force *= curl * C;
    force.y *= -1.0;
    
    float2 vel = uVelocity.Sample(linearClampSampler, i.uv).xy;
    return float4(vel + force * dt, 0.0, 1.0);
}

// --- PASS 4: DIVERGENCE ---
// Measures the divergence of the velocity field
float4 PS_Divergence(v2f i) : SV_Target {
    float L = uVelocity.Sample(linearClampSampler, i.vL).x;
    float R = uVelocity.Sample(linearClampSampler, i.vR).x;
    float B = uVelocity.Sample(linearClampSampler, i.vB).y;
    float T = uVelocity.Sample(linearClampSampler, i.vT).y;
    float2 C = uVelocity.Sample(linearClampSampler, i.uv).xy;
    
    // Boundary conditions
    if (i.vL.x < 0.0) { L = -C.x; }
    if (i.vR.x > 1.0) { R = -C.x; }
    if (i.vB.y < 0.0) { B = -C.y; }
    if (i.vT.y > 1.0) { T = -C.y; }
    
    float div = 0.5 * (R - L + T - B);
    return float4(div, 0.0, 0.0, 1.0);
}

// --- PASS 5: PRESSURE (Iterative Jacobi) ---
// Run this pass 20-50 times, ping-ponging the pressure texture
float4 PS_Pressure(v2f i) : SV_Target {
    float L = uPressure.Sample(linearClampSampler, i.vL).x;
    float R = uPressure.Sample(linearClampSampler, i.vR).x;
    float B = uPressure.Sample(linearClampSampler, i.vB).x;
    float T = uPressure.Sample(linearClampSampler, i.vT).x;
    
    float divergence = uDivergence.Sample(linearClampSampler, i.uv).x;
    float pressure = (L + R + B + T - divergence) * 0.25;
    
    return float4(pressure, 0.0, 0.0, 1.0);
}

// --- PASS 6: GRADIENT SUBTRACT ---
// Forces the velocity field to be divergence-free
float4 PS_GradientSubtract(v2f i) : SV_Target {
    float L = uPressure.Sample(linearClampSampler, i.vL).x;
    float R = uPressure.Sample(linearClampSampler, i.vR).x;
    float B = uPressure.Sample(linearClampSampler, i.vB).x;
    float T = uPressure.Sample(linearClampSampler, i.vT).x;
    
    float2 velocity = uVelocity.Sample(linearClampSampler, i.uv).xy;
    velocity.xy -= float2(R - L, T - B);
    
    return float4(velocity, 0.0, 1.0);
}

// --- PASS 7: MOUSE / POINTER SPLAT ---
// Injects mouse data into the simulation.
// Run this twice per frame when the mouse moves: 
// 1. Splat velocity (splatColor = mouseDelta * force) -> Write to uVelocity
// 2. Splat dye (splatColor = color) -> Write to tMap
float segmentDistance(float2 uv, float2 a, float2 b) {
    float2 pa = uv - a;
    float2 ba = b - a;
    pa.x *= aspectRatio;
    ba.x *= aspectRatio;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
    return length(pa - ba * h);
}

float4 PS_Splat(v2f i) : SV_Target {
    float d = segmentDistance(i.uv, prevPointPos, pointPos);
    float weight = exp(-(d * d) / max(splatRadius, 1e-6));
    float3 splat = weight * splatColor;
    float4 base = uTarget.Sample(linearClampSampler, i.uv);
    
    float coverage = saturate(weight * splatOpacity);
    return float4(base.rgb + splat, coverage + base.a * (1.0 - coverage));
}

// ============================================================================
// FINAL COMPOSITE (DEVELOPER PORTION UI)
// ============================================================================

// Applies bias curve mirroring Shopify's React component properties
float gradientMix(float x, float bias) {
    return smoothstep(0.0, 1.0, pow(clamp(x, 0.0, 1.0), max(bias, 1e-4)));
}

// Calculates linear gradient angular slice
float linearGradientPosition(float2 uv) {
    float s, c;
    sincos(uBackgroundAngle, s, c);
    float2 dir = float2(c, s);
    return saturate(dot(uv - 0.5, dir) + 0.5);
}

// Generates the exact 3-stop gray styled gradient background
float3 backgroundColor(float2 uv) {
    if (uBackgroundMode == 1.0) return uBackgroundColor1;
    
    float pos = linearGradientPosition(uv);
    
    if (uBackgroundPointCount < 3.0) {
        return lerp(uBackgroundColor1, uBackgroundColor3, gradientMix(pos, uBackgroundBias1));
    }
    
    if (pos < 0.5) {
        return lerp(uBackgroundColor1, uBackgroundColor2, gradientMix(pos * 2.0, uBackgroundBias1));
    }
    
    return lerp(uBackgroundColor2, uBackgroundColor3, gradientMix((pos - 0.5) * 2.0, uBackgroundBias2));
}

float3 linearToSRGB(float3 col) {
    return pow(max(col, 0.0), 1.0 / 2.2);
}

// --- PASS 8: DISPLAY COMPOSITE ---
// Composites the simulated fluid dye over the designer's styled gray gradient
float4 PS_Composite(v2f i) : SV_Target {
    // 1. Get the fluid dye color (from tMap generated by the simulation)
    float4 fluidColor = tMap.Sample(linearClampSampler, i.uv);
    
    // 2. Generate the background gradient natively in shader
    float3 bg = linearToSRGB(backgroundColor(i.uv));
    bg *= 1.0 - clamp(uBackgroundDarken, 0.0, 1.0);
    
    // 3. Blend fluid over background (standard alpha blending matching the LDR look designers chose)
    float3 finalColor = lerp(bg, fluidColor.rgb, saturate(fluidColor.a));
    
    return float4(finalColor, 1.0);
}
