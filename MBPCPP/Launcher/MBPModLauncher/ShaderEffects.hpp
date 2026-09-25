#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

class ShaderEffects
{
public:
    ShaderEffects() = default;
    ~ShaderEffects();

    bool Initialize(ID3D12Device* device, DXGI_FORMAT targetFormat, unsigned frameCount);
    void Shutdown();

    void RenderLoading(
        ID3D12GraphicsCommandList* commandList,
        unsigned width,
        unsigned height,
        float timeSeconds,
        float opacity,
        bool cppSelected,
        unsigned frameIndex);

private:
    struct alignas(16) LoadingConstants
    {
        float params[4]{};
        float shape0[4]{};
        float shape1[4]{};
        float tint[4]{};
    };

    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> loadingPipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> glowPipeline_;
    Microsoft::WRL::ComPtr<ID3D12Resource> constantBuffer_;
    std::uint8_t* mappedConstants_ = nullptr;
    unsigned frameCount_ = 0;
    unsigned constantStride_ = 256;
};
