#include "DX12Renderer.hpp"
#include "ShaderEffects.hpp"

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <cassert>

using Microsoft::WRL::ComPtr;

DX12Renderer::~DX12Renderer()
{
    Shutdown();
}

void DX12Renderer::DescriptorAllocator::Create(ID3D12Device* device, ID3D12DescriptorHeap* heap)
{
    heap_ = heap;
    const D3D12_DESCRIPTOR_HEAP_DESC desc = heap_->GetDesc();
    type_ = desc.Type;
    cpuStart_ = heap_->GetCPUDescriptorHandleForHeapStart();
    gpuStart_ = heap_->GetGPUDescriptorHandleForHeapStart();
    increment_ = device->GetDescriptorHandleIncrementSize(type_);
    free_.clear();
    free_.reserve(desc.NumDescriptors);
    for (int i = static_cast<int>(desc.NumDescriptors) - 1; i >= 0; --i)
        free_.push_back(i);
}

void DX12Renderer::DescriptorAllocator::Destroy()
{
    heap_ = nullptr;
    free_.clear();
}

bool DX12Renderer::DescriptorAllocator::Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    if (!heap_ || free_.empty() || !cpu || !gpu)
        return false;
    const int index = free_.back();
    free_.pop_back();
    cpu->ptr = cpuStart_.ptr + static_cast<SIZE_T>(index) * increment_;
    gpu->ptr = gpuStart_.ptr + static_cast<UINT64>(index) * increment_;
    return true;
}

void DX12Renderer::DescriptorAllocator::Free(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
{
    if (!heap_ || increment_ == 0)
        return;
    const int cpuIndex = static_cast<int>((cpu.ptr - cpuStart_.ptr) / increment_);
    const int gpuIndex = static_cast<int>((gpu.ptr - gpuStart_.ptr) / increment_);
    if (cpuIndex == gpuIndex && cpuIndex >= 0)
        free_.push_back(cpuIndex);
}

void DX12Renderer::ImGuiSrvAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    auto* self = static_cast<DX12Renderer*>(info->UserData);
    if (!self || !self->srvAllocator_.Alloc(cpu, gpu))
    {
        if (cpu) cpu->ptr = 0;
        if (gpu) gpu->ptr = 0;
    }
}

void DX12Renderer::ImGuiSrvFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
{
    if (auto* self = static_cast<DX12Renderer*>(info->UserData))
        self->srvAllocator_.Free(cpu, gpu);
}

bool DX12Renderer::Initialize(HWND hwnd)
{
    hwnd_ = hwnd;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    width_ = std::max<LONG>(1, rc.right - rc.left);
    height_ = std::max<LONG>(1, rc.bottom - rc.top);

#ifdef _DEBUG
    if (ComPtr<ID3D12Debug> debug; SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();
#endif

    ComPtr<IDXGIFactory6> factory;
    UINT factoryFlags = 0;
#ifdef _DEBUG
    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory))))
        return false;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            adapter.Reset();
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_))))
            break;
        adapter.Reset();
    }
    if (!device_ && FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_))))
        return false;

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue_))))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = BackBufferCount;
    if (FAILED(device_->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_))))
        return false;

    const UINT rtvIncrement = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < BackBufferCount; ++i)
    {
        rtvHandles_[i] = rtv;
        rtv.ptr += rtvIncrement;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = SrvHeapSize;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap_))))
        return false;
    srvAllocator_.Create(device_.Get(), srvHeap_.Get());

    for (FrameContext& frame : frameContexts_)
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator))))
            return false;

    if (FAILED(device_->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        frameContexts_[0].allocator.Get(),
        nullptr,
        IID_PPV_ARGS(&commandList_))))
        return false;
    commandList_->Close();

    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
        return false;
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_)
        return false;

    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.BufferCount = BackBufferCount;
    swapDesc.Width = width_;
    swapDesc.Height = height_;
    swapDesc.Format = backBufferFormat_;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapDesc.Scaling = DXGI_SCALING_STRETCH;
    swapDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGISwapChain1> swap1;
    if (FAILED(factory->CreateSwapChainForHwnd(commandQueue_.Get(), hwnd_, &swapDesc, nullptr, nullptr, &swap1)))
        return false;
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(swap1.As(&swapChain_)))
        return false;

    swapChain_->SetMaximumFrameLatency(BackBufferCount);
    swapChainWaitableObject_ = swapChain_->GetFrameLatencyWaitableObject();
    if (!swapChainWaitableObject_)
        return false;

    return CreateRenderTargets();
}

bool DX12Renderer::InitializeImGui(HWND hwnd)
{
    if (!device_ || !commandQueue_ || !srvHeap_)
        return false;
    if (!ImGui_ImplWin32_Init(hwnd))
        return false;

    ImGui_ImplDX12_InitInfo init{};
    init.Device = device_.Get();
    init.CommandQueue = commandQueue_.Get();
    init.NumFramesInFlight = FramesInFlight;
    init.RTVFormat = backBufferFormat_;
    init.DSVFormat = DXGI_FORMAT_UNKNOWN;
    init.UserData = this;
    init.SrvDescriptorHeap = srvHeap_.Get();
    init.SrvDescriptorAllocFn = &DX12Renderer::ImGuiSrvAlloc;
    init.SrvDescriptorFreeFn = &DX12Renderer::ImGuiSrvFree;

    if (!ImGui_ImplDX12_Init(&init))
    {
        ImGui_ImplWin32_Shutdown();
        return false;
    }
    imguiInitialized_ = true;
    return true;
}

bool DX12Renderer::CreateRenderTargets()
{
    for (UINT i = 0; i < BackBufferCount; ++i)
    {
        if (FAILED(swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i]))))
            return false;
        device_->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, rtvHandles_[i]);
    }
    return true;
}

void DX12Renderer::DestroyRenderTargets()
{
    for (auto& buffer : backBuffers_)
        buffer.Reset();
}

void DX12Renderer::BeginImGuiFrame()
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

DX12Renderer::FrameContext* DX12Renderer::WaitForNextFrameContext()
{
    FrameContext& frame = frameContexts_[frameIndex_ % FramesInFlight];
    ++frameIndex_;

    HANDLE waitables[] = { swapChainWaitableObject_, nullptr };
    DWORD count = 1;

    if (frame.fenceValue != 0)
    {
        fence_->SetEventOnCompletion(frame.fenceValue, fenceEvent_);
        waitables[1] = fenceEvent_;
        count = 2;
    }

    WaitForMultipleObjects(count, waitables, TRUE, INFINITE);
    frame.fenceValue = 0;
    return &frame;
}

bool DX12Renderer::RenderFrame(ShaderEffects& effects, float timeSeconds, bool cppSelected, float opacity)
{
    ImGui::Render();
    FrameContext* frame = WaitForNextFrameContext();
    const UINT backBufferIndex = swapChain_->GetCurrentBackBufferIndex();

    if (FAILED(frame->allocator->Reset()))
        return false;
    if (FAILED(commandList_->Reset(frame->allocator.Get(), nullptr)))
        return false;

    D3D12_RESOURCE_BARRIER toRender{};
    toRender.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRender.Transition.pResource = backBuffers_[backBufferIndex].Get();
    toRender.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRender.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRender.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList_->ResourceBarrier(1, &toRender);

    const float clear[4] = { 0.004f, 0.006f, 0.014f, 1.0f };
    commandList_->OMSetRenderTargets(1, &rtvHandles_[backBufferIndex], FALSE, nullptr);
    commandList_->ClearRenderTargetView(rtvHandles_[backBufferIndex], clear, 0, nullptr);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
    commandList_->RSSetViewports(1, &viewport);
    commandList_->RSSetScissorRects(1, &scissor);

    effects.RenderLoading(commandList_.Get(), width_, height_, timeSeconds, opacity, cppSelected, frameIndex_ % FramesInFlight);

    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    commandList_->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList_.Get());

    std::swap(toRender.Transition.StateBefore, toRender.Transition.StateAfter);
    commandList_->ResourceBarrier(1, &toRender);
    if (FAILED(commandList_->Close()))
        return false;

    ID3D12CommandList* lists[] = { commandList_.Get() };
    commandQueue_->ExecuteCommandLists(1, lists);

    const UINT64 signaled = ++fenceLastSignaled_;
    if (FAILED(commandQueue_->Signal(fence_.Get(), signaled)))
        return false;
    frame->fenceValue = signaled;

    const HRESULT present = swapChain_->Present(1, 0);
    occluded_ = present == DXGI_STATUS_OCCLUDED;
    return SUCCEEDED(present) || present == DXGI_STATUS_OCCLUDED;
}

void DX12Renderer::Resize(UINT width, UINT height)
{
    if (!swapChain_ || width == 0 || height == 0 || (width == width_ && height == height_))
        return;

    WaitForPendingOperations();
    DestroyRenderTargets();
    width_ = width;
    height_ = height;

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain_->GetDesc(&desc)))
        return;
    if (FAILED(swapChain_->ResizeBuffers(BackBufferCount, width_, height_, backBufferFormat_, desc.Flags)))
        return;
    CreateRenderTargets();
}

void DX12Renderer::WaitForPendingOperations()
{
    if (!commandQueue_ || !fence_ || !fenceEvent_)
        return;
    const UINT64 value = ++fenceLastSignaled_;
    if (SUCCEEDED(commandQueue_->Signal(fence_.Get(), value)) && fence_->GetCompletedValue() < value)
    {
        if (SUCCEEDED(fence_->SetEventOnCompletion(value, fenceEvent_)))
            WaitForSingleObject(fenceEvent_, INFINITE);
    }
    for (FrameContext& frame : frameContexts_)
        frame.fenceValue = 0;
}

UINT DX12Renderer::CurrentBackBufferIndex() const noexcept
{
    return swapChain_ ? swapChain_->GetCurrentBackBufferIndex() : 0;
}

void DX12Renderer::Shutdown()
{
    if (!device_ && !swapChain_)
        return;

    WaitForPendingOperations();

    if (imguiInitialized_)
    {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        imguiInitialized_ = false;
    }

    DestroyRenderTargets();
    srvAllocator_.Destroy();

    if (swapChainWaitableObject_)
    {
        CloseHandle(swapChainWaitableObject_);
        swapChainWaitableObject_ = nullptr;
    }
    if (fenceEvent_)
    {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }

    swapChain_.Reset();
    fence_.Reset();
    commandList_.Reset();
    for (auto& frame : frameContexts_) frame.allocator.Reset();
    srvHeap_.Reset();
    rtvHeap_.Reset();
    commandQueue_.Reset();
    device_.Reset();
}
