#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <vector>

class ShaderEffects;

class DX12Renderer
{
public:
    static constexpr UINT FramesInFlight = 3;
    static constexpr UINT BackBufferCount = 3;
    static constexpr UINT SrvHeapSize = 64;

    DX12Renderer() = default;
    ~DX12Renderer();

    DX12Renderer(const DX12Renderer&) = delete;
    DX12Renderer& operator=(const DX12Renderer&) = delete;

    bool Initialize(HWND hwnd);
    bool InitializeImGui(HWND hwnd);
    void Shutdown();

    void BeginImGuiFrame();
    bool RenderFrame(ShaderEffects& effects, float timeSeconds, bool cppSelected, float opacity = 1.0f);
    void Resize(UINT width, UINT height);
    void WaitForPendingOperations();

    ID3D12Device* Device() const noexcept { return device_.Get(); }
    ID3D12CommandQueue* CommandQueue() const noexcept { return commandQueue_.Get(); }
    DXGI_FORMAT BackBufferFormat() const noexcept { return backBufferFormat_; }
    UINT CurrentBackBufferIndex() const noexcept;
    UINT Width() const noexcept { return width_; }
    UINT Height() const noexcept { return height_; }
    bool IsOccluded() const noexcept { return occluded_; }

private:
    struct FrameContext
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        UINT64 fenceValue = 0;
    };

    class DescriptorAllocator
    {
    public:
        void Create(ID3D12Device* device, ID3D12DescriptorHeap* heap);
        void Destroy();
        bool Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
        void Free(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);

    private:
        ID3D12DescriptorHeap* heap_ = nullptr;
        D3D12_DESCRIPTOR_HEAP_TYPE type_ = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuStart_{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpuStart_{};
        UINT increment_ = 0;
        std::vector<int> free_;
    };

    static void ImGuiSrvAlloc(struct ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
    static void ImGuiSrvFree(struct ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);

    bool CreateRenderTargets();
    void DestroyRenderTargets();
    FrameContext* WaitForNextFrameContext();

    HWND hwnd_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT frameIndex_ = 0;
    UINT64 fenceLastSignaled_ = 0;
    bool occluded_ = false;
    bool imguiInitialized_ = false;

    DXGI_FORMAT backBufferFormat_ = DXGI_FORMAT_R8G8B8A8_UNORM;

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_;

    std::array<FrameContext, FramesInFlight> frameContexts_{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, BackBufferCount> backBuffers_{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, BackBufferCount> rtvHandles_{};

    DescriptorAllocator srvAllocator_;
    HANDLE fenceEvent_ = nullptr;
    HANDLE swapChainWaitableObject_ = nullptr;
};
