#include "ShaderEffects.hpp"
#include "ShaderLoader.h"

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

ShaderEffects::~ShaderEffects()
{
    Shutdown();
}

bool ShaderEffects::Initialize(ID3D12Device* device, DXGI_FORMAT targetFormat, unsigned frameCount)
{
    if (!device || frameCount == 0)
        return false;
    frameCount_ = frameCount;

    D3D12_ROOT_PARAMETER rootParameter{};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameter.Descriptor.ShaderRegister = 0;
    rootParameter.Descriptor.RegisterSpace = 0;
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &rootParameter;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
        return false;
    if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&rootSignature_))))
        return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSignature_.Get();
    pso.VS = ShaderLoader::Load("fullscreen_vs");
    pso.PS = ShaderLoader::Load("loading_ps");
    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC blend{};
    blend.BlendEnable = FALSE;
    blend.LogicOpEnable = FALSE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.BlendState.RenderTarget[0] = blend;

    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    pso.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pso.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.MultisampleEnable = FALSE;
    pso.RasterizerState.AntialiasedLineEnable = FALSE;
    pso.RasterizerState.ForcedSampleCount = 0;
    pso.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.InputLayout = { nullptr, 0 };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = targetFormat;
    pso.SampleDesc.Count = 1;

    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&loadingPipeline_))))
        return false;

    // A second full-screen pass adds a restrained procedural halo behind the
    // selected player card and launch control. It uses the same root signature
    // and embedded fullscreen vertex shader, but alpha blends over Loading.hlsl.
    pso.PS = ShaderLoader::Load("launcher_glow_ps");
    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&glowPipeline_))))
        return false;

    const UINT64 bufferSize = static_cast<UINT64>(constantStride_) * frameCount_ * 2ull;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource.Width = bufferSize;
    resource.Height = 1;
    resource.DepthOrArraySize = 1;
    resource.MipLevels = 1;
    resource.SampleDesc.Count = 1;
    resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &resource,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&constantBuffer_))))
        return false;

    if (FAILED(constantBuffer_->Map(0, nullptr, reinterpret_cast<void**>(&mappedConstants_))))
        return false;
    return true;
}

void ShaderEffects::RenderLoading(
    ID3D12GraphicsCommandList* commandList,
    unsigned width,
    unsigned height,
    float timeSeconds,
    float opacity,
    bool cppSelected,
    unsigned frameIndex)
{
    if (!commandList || !loadingPipeline_ || !glowPipeline_ || !rootSignature_ || !constantBuffer_ || !mappedConstants_ || frameCount_ == 0)
        return;

    const unsigned slot = frameIndex % frameCount_;
    const size_t baseOffset = static_cast<size_t>(slot) * constantStride_ * 2u;
    LoadingConstants constants{};
    constants.params[0] = timeSeconds;
    constants.params[1] = static_cast<float>(std::max(width, 1u));
    constants.params[2] = static_cast<float>(std::max(height, 1u));
    constants.params[3] = std::clamp(opacity, 0.0f, 1.0f);

    std::memcpy(mappedConstants_ + baseOffset, &constants, sizeof(constants));
    const D3D12_GPU_VIRTUAL_ADDRESS loadingAddress = constantBuffer_->GetGPUVirtualAddress() + baseOffset;

    commandList->SetGraphicsRootSignature(rootSignature_.Get());
    commandList->SetPipelineState(loadingPipeline_.Get());
    commandList->SetGraphicsRootConstantBufferView(0, loadingAddress);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);

    LoadingConstants glow{};
    glow.params[0] = timeSeconds;
    glow.params[1] = constants.params[1];
    glow.params[2] = constants.params[2];

    const float shellWidth = std::min(760.0f, constants.params[1] - 72.0f);
    const float shellX = (constants.params[1] - shellWidth) * 0.5f;
    const float cardGap = 18.0f;
    const float cardWidth = (shellWidth - 68.0f - cardGap) * 0.5f;
    const float selectedCenterX = shellX + 34.0f + cardWidth * 0.5f + (cppSelected ? 0.0f : cardWidth + cardGap);
    glow.shape0[0] = selectedCenterX;
    glow.shape0[1] = 104.0f + 98.0f + 77.0f;
    glow.shape0[2] = cardWidth * 0.5f;
    glow.shape0[3] = 77.0f;
    glow.shape1[0] = constants.params[1] * 0.5f;
    glow.shape1[1] = constants.params[2] - 174.0f + 28.0f;
    glow.shape1[2] = 119.0f;
    glow.shape1[3] = 28.0f;
    glow.tint[0] = cppSelected ? 0.18f : 0.50f;
    glow.tint[1] = cppSelected ? 0.48f : 0.24f;
    glow.tint[2] = cppSelected ? 0.98f : 0.96f;
    glow.tint[3] = 0.18f;

    const size_t glowOffset = baseOffset + constantStride_;
    std::memcpy(mappedConstants_ + glowOffset, &glow, sizeof(glow));
    const D3D12_GPU_VIRTUAL_ADDRESS glowAddress = constantBuffer_->GetGPUVirtualAddress() + glowOffset;
    commandList->SetPipelineState(glowPipeline_.Get());
    commandList->SetGraphicsRootConstantBufferView(0, glowAddress);
    commandList->DrawInstanced(3, 1, 0, 0);
}

void ShaderEffects::Shutdown()
{
    if (constantBuffer_ && mappedConstants_)
        constantBuffer_->Unmap(0, nullptr);
    mappedConstants_ = nullptr;
    constantBuffer_.Reset();
    glowPipeline_.Reset();
    loadingPipeline_.Reset();
    rootSignature_.Reset();
    frameCount_ = 0;
}
