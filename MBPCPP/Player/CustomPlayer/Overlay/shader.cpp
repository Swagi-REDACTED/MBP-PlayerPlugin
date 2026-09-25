#include "main.hpp"
#include <d3dcompiler.h>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

// --- D3D11 SHADER STATE ---
static ID3D11PixelShader* s_pBlurShader = nullptr;
static ID3D11PixelShader* s_pGlowShader = nullptr;       // legacy glow (kept intentionally)
static ID3D11PixelShader* s_pMistGlowShader = nullptr;   // new animated smoke/mist glow
static ID3D11PixelShader* s_pLoadingShader = nullptr;
static ID3D11Buffer* s_pConstantBuffer = nullptr;
static ID3D11Texture2D* s_pScreenTexture = nullptr;
static ID3D11ShaderResourceView* s_pScreenSRV = nullptr;
static ImVec2 s_ScreenSize = { 0, 0 };

struct ShaderConstants {
    float params[4]; // x=intensity/radius/time, y=screenWidth, z=screenHeight, w=time/opacity
    float shape0[4]; // x=centerX, y=centerY, z=halfWidth, w=halfHeight
    float shape1[4]; // x=roundness/radius, y=feather, z=shapeMode, w=reserved
    float color[4];  // r, g, b, a (used for tinting/emissive core)
};

struct EffectState {
    ShaderConstants constants;
    int type; // 1 = Blur, 2 = Legacy Glow, 3 = Procedural loading background, 4 = Mist Glow
};
static std::vector<EffectState> s_EffectStates;

// --- HLSL SOURCE STRINGS ---
static const char* glowShaderSrc = R"(
cbuffer Constants : register(b0) {
    float4 params;
    float4 shape0;
    float4 shape1;
    float4 glowColor;
};
struct PS_INPUT {
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};
sampler sampler0 : register(s0);
Texture2D texture0 : register(t0);

float4 main(PS_INPUT input) : SV_Target {
    float4 texCol = texture0.Sample(sampler0, input.uv);
    float4 baseCol = input.col * texCol;
    
    // params.x controls HDR bloom intensity
    float intensity = params.x;
    
    // Additive emissive glow applied uniformly across the alpha bounds
    float3 glow = glowColor.rgb * intensity;
    
    return float4(baseCol.rgb + (glow * baseCol.a), baseCol.a);
}
)";

static const char* blurShaderSrc = R"(
cbuffer Constants : register(b0) {
    float4 params; 
    float4 shape0;
    float4 shape1;
    float4 bgColor;
};
struct PS_INPUT {
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};
sampler sampler0 : register(s0);
Texture2D screenTex : register(t1); // Map to t1 to avoid ImGui font atlas conflicts

float4 main(PS_INPUT input) : SV_Target {
    float2 uv = input.pos.xy / float2(params.y, params.z);
    float radius = params.x;
    
    float4 color = float4(0,0,0,0);
    float totalWeight = 0.0;
    float2 texelSize = 1.0 / float2(params.y, params.z);
    
    // 81-tap High-Quality Gaussian Blur (WebKit Match)
    float sigma = radius * 0.5;
    float twoSigmaSq = 2.0 * sigma * sigma;
    
    for(int x = -4; x <= 4; x++) {
        for(int y = -4; y <= 4; y++) {
            float2 offset = float2(x, y);
            float weight = exp(-(offset.x*offset.x + offset.y*offset.y) / twoSigmaSq);
            
            // Multiply radius scaling to spread the samples correctly
            float2 sampleUV = uv + offset * texelSize * (radius / 4.0);
            color += screenTex.SampleLevel(sampler0, sampleUV, 0) * weight;
            totalWeight += weight;
        }
    }
    color /= totalWeight;
    
    // Accurate alpha blending matching WebKit CSS behavior (backdrop + overlay)
    float4 result = float4(lerp(color.rgb, bgColor.rgb, bgColor.a), 1.0);
    return result * input.col;
}
)";


// Animated smoke/mist emissive glow. This intentionally lives beside the old
// glowShaderSrc so the previous implementation remains available as a fallback.
static const char* mistGlowShaderSrc = R"(
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
)";


// Fullscreen procedural loader. No textures/assets are required: this is a
// small domain-warped aurora/caustic field designed to sit naturally behind
// the player's glass UI rather than looking like a generic spinner.
static const char* loadingShaderSrc = R"(
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
)";

// Initialize shaders, compiling them at runtime via D3DCompile
void InitShaders() {
    if (!g_pd3dDevice) return;

    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;

    // Compile Glow
    if (SUCCEEDED(D3DCompile(glowShaderSrc, strlen(glowShaderSrc), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &blob, &err))) {
        g_pd3dDevice->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_pGlowShader);
        blob->Release();
    }

    // Compile the new mist/smoke glow. The legacy glow above remains intact.
    blob = nullptr;
    if (err) { err->Release(); err = nullptr; }
    if (SUCCEEDED(D3DCompile(mistGlowShaderSrc, strlen(mistGlowShaderSrc), nullptr, nullptr, nullptr, "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err))) {
        g_pd3dDevice->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_pMistGlowShader);
        blob->Release();
        blob = nullptr;
    }
    else if (err) {
        OutputDebugStringA("CustomPlayer mist glow shader compile error: ");
        OutputDebugStringA((const char*)err->GetBufferPointer());
        OutputDebugStringA("\n");
    }
    if (err) { err->Release(); err = nullptr; }

    // Compile Blur
    if (SUCCEEDED(D3DCompile(blurShaderSrc, strlen(blurShaderSrc), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &blob, &err))) {
        g_pd3dDevice->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_pBlurShader);
        blob->Release();
    }

    // Compile the procedural fullscreen loading background.
    blob = nullptr;
    if (err) { err->Release(); err = nullptr; }
    if (SUCCEEDED(D3DCompile(loadingShaderSrc, strlen(loadingShaderSrc), nullptr, nullptr, nullptr, "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err))) {
        g_pd3dDevice->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_pLoadingShader);
        blob->Release();
        blob = nullptr;
    }
    else if (err) {
        OutputDebugStringA("CustomPlayer loading shader compile error: ");
        OutputDebugStringA((const char*)err->GetBufferPointer());
        OutputDebugStringA("\n");
    }
    if (err) { err->Release(); err = nullptr; }

    // Create Constant Buffer
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(ShaderConstants);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_pd3dDevice->CreateBuffer(&desc, nullptr, &s_pConstantBuffer);
}

// Cleans up states, meant to be called at the start of each frame
void ClearShaderState() {
    s_EffectStates.clear();
}

// Updates our snapshot of the Backbuffer to be used in the Blur pass
void UpdateScreenTexture(ImVec2 size) {
    if (s_ScreenSize.x == size.x && s_ScreenSize.y == size.y && s_pScreenTexture) return;

    if (s_pScreenSRV) { s_pScreenSRV->Release(); s_pScreenSRV = nullptr; }
    if (s_pScreenTexture) { s_pScreenTexture->Release(); s_pScreenTexture = nullptr; }

    s_ScreenSize = size;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = (UINT)size.x;
    desc.Height = (UINT)size.y;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    g_pd3dDevice->CreateTexture2D(&desc, nullptr, &s_pScreenTexture);
    if (s_pScreenTexture) {
        g_pd3dDevice->CreateShaderResourceView(s_pScreenTexture, nullptr, &s_pScreenSRV);
    }
}

// General function to push an HLSL pipeline change onto the ImGui draw list
static void PushEffect(
    ImDrawList* drawList,
    int type,
    float p1,
    float p2,
    float p3,
    float p4,
    ImVec4 col,
    ImVec4 shape0 = ImVec4(0, 0, 0, 0),
    ImVec4 shape1 = ImVec4(0, 0, 0, 0)) {
    ShaderConstants constants = {
        {p1, p2, p3, p4},
        {shape0.x, shape0.y, shape0.z, shape0.w},
        {shape1.x, shape1.y, shape1.z, shape1.w},
        {col.x, col.y, col.z, col.w}
    };
    s_EffectStates.push_back({ constants, type });

    drawList->AddCallback([](const ImDrawList* parent_list, const ImDrawCmd* cmd) {
        int index = (int)(intptr_t)cmd->UserCallbackData;
        if (index < 0 || index >= s_EffectStates.size() || !g_pd3dDeviceContext) return;
        auto& state = s_EffectStates[index];

        // Map Constant buffer variables
        D3D11_MAPPED_SUBRESOURCE mapped;
        g_pd3dDeviceContext->Map(s_pConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        memcpy(mapped.pData, &state.constants, sizeof(ShaderConstants));
        g_pd3dDeviceContext->Unmap(s_pConstantBuffer, 0);

        g_pd3dDeviceContext->PSSetConstantBuffers(0, 1, &s_pConstantBuffer);

        if (state.type == 1) { // Blur Pass
            ID3D11RenderTargetView* rtv = nullptr;
            g_pd3dDeviceContext->OMGetRenderTargets(1, &rtv, nullptr);
            if (rtv) {
                ID3D11Resource* res = nullptr;
                rtv->GetResource(&res);
                if (res) {
                    // Copy the current screen state over to our background texture
                    g_pd3dDeviceContext->CopyResource(s_pScreenTexture, res);
                    res->Release();
                }
                rtv->Release();
            }
            g_pd3dDeviceContext->PSSetShader(s_pBlurShader, nullptr, 0);
            g_pd3dDeviceContext->PSSetShaderResources(1, 1, &s_pScreenSRV);
        }
        else if (state.type == 2) { // Glow Emissive Pass
            g_pd3dDeviceContext->PSSetShader(s_pGlowShader, nullptr, 0);
        }
        else if (state.type == 3) { // Procedural fullscreen loading pass
            g_pd3dDeviceContext->PSSetShader(s_pLoadingShader, nullptr, 0);
        }
        else if (state.type == 4) { // Animated mist/smoke glow
            g_pd3dDeviceContext->PSSetShader(s_pMistGlowShader, nullptr, 0);
        }
        }, (void*)(intptr_t)(s_EffectStates.size() - 1));
}

// Restores default ImGui pipeline rendering
static void PopEffect(ImDrawList* drawList) {
    drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

// --- PUBLIC EXPORTS ---

void PushGlow(ImDrawList* drawList, ImU32 glowColor, float intensity) {
    if (!s_pGlowShader) return;
    ImVec4 col = ImGui::ColorConvertU32ToFloat4(glowColor);
    PushEffect(drawList, 2, intensity, 0, 0, 0, col);
}

void PopGlow(ImDrawList* drawList) {
    if (!s_pGlowShader) return;
    PopEffect(drawList);
}

void PushMistGlowShape(
    ImDrawList* drawList,
    ImU32 glowColor,
    float intensity,
    float timeSeconds,
    ImVec2 center,
    ImVec2 halfSize,
    float roundness,
    float feather,
    float shapeMode) {
    if (!drawList) return;

    // If the new shader failed to compile on an older/odd driver, retain the
    // legacy glow behavior instead of losing the effect entirely.
    if (!s_pMistGlowShader) {
        PushGlow(drawList, glowColor, intensity);
        return;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImVec4 col = ImGui::ColorConvertU32ToFloat4(glowColor);
    PushEffect(
        drawList,
        4,
        intensity,
        display.x,
        display.y,
        timeSeconds,
        col,
        ImVec4(center.x, center.y, halfSize.x, halfSize.y),
        ImVec4(roundness, feather, shapeMode, 0.0f));
}

void PushMistGlow(ImDrawList* drawList, ImU32 glowColor, float intensity, float timeSeconds) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    PushMistGlowShape(
        drawList,
        glowColor,
        intensity,
        timeSeconds,
        ImVec2(display.x * 0.5f, display.y * 0.5f),
        ImVec2(display.x * 0.5f, display.y * 0.5f),
        32.0f,
        48.0f,
        0.0f);
}

void PopMistGlow(ImDrawList* drawList) {
    if (!drawList) return;
    PopEffect(drawList);
}

void DrawBlurRect(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImVec4 overlayColor, float blurRadius) {
    if (!s_pBlurShader || blurRadius <= 0.0f) { // Fallback if shaders fail or unrequested
        drawList->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(overlayColor));
        return;
    }
    UpdateScreenTexture(ImGui::GetIO().DisplaySize);
    PushEffect(drawList, 1, blurRadius, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y, 0.0f, overlayColor);

    // We draw pure white so vertex coloring doesn't tint it further (tint is managed by the overlayColor params)
    drawList->AddRectFilled(min, max, IM_COL32_WHITE);
    PopEffect(drawList);
}

// Draws the fullscreen procedural loader through the existing ImGui callback
// pipeline. The rectangle is only geometry; all visible color comes from HLSL.
void DrawLoadingShader(ImDrawList* drawList, ImVec2 min, ImVec2 max, float timeSeconds, float opacity) {
    if (!drawList) return;

    opacity = std::clamp(opacity, 0.0f, 1.0f);
    if (!s_pLoadingShader || !s_pConstantBuffer) {
        drawList->AddRectFilled(min, max, IM_COL32(8, 10, 20, (int)(255.0f * opacity)));
        return;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    PushEffect(drawList, 3, timeSeconds, display.x, display.y, opacity, ImVec4(0, 0, 0, 0));
    drawList->AddRectFilled(min, max, IM_COL32_WHITE);
    PopEffect(drawList);
}
