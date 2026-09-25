#include "videoengine.hpp"
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h> // FIX: The header containing ID3D11Multithread for UI Thread Protection!
#include <d3dcompiler.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <wincodec.h> // WIC integration for robust software frame extraction
#include <wrl/client.h>
#include <cmath>
#include <string>
#include <algorithm> // For std::clamp and std::max
#include <vector>
#include <atomic>    // For cross-thread MF callbacks
#include <cstdio>

// Link necessary modern hardware multimedia libraries
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib") // WIC Library
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

using Microsoft::WRL::ComPtr;

// External DX11 Device contexts from the main application (used by ImGui)
extern ID3D11Device* g_pd3dDevice;
extern ID3D11DeviceContext* g_pd3dDeviceContext;

// --------------------------------------------------------------------------------------
// CUSTOM DX11 PIPELINE STATE
// --------------------------------------------------------------------------------------
static bool g_mfInitialized = false;
static ComPtr<IMFMediaEngine> g_mediaEngine;
static ComPtr<IMFDXGIDeviceManager> g_dxgiManager;
static UINT g_resetToken = 0;

// WIC Software Pipeline States
static ComPtr<IWICImagingFactory> g_wicFactory;
static ComPtr<IWICBitmap> g_wicBitmap;

// Thread-safe event states for Media Foundation worker threads
static std::atomic<bool> g_metadataReady(false);
static std::atomic<bool> g_videoFormatChanged(false);


// Playback state is written by Media Foundation worker threads and read by the UI thread.
enum class NativePlayerState : int {
    Idle = 0,
    Loading,
    Ready,
    Playing,
    Paused,
    Buffering,
    Ended,
    Error
};

static std::atomic<NativePlayerState> g_playerState(NativePlayerState::Idle);
static std::atomic<DWORD> g_lastMediaError(MF_MEDIA_ENGINE_ERR_NOERROR);
static std::atomic<LONG> g_lastMediaHr(S_OK);
static std::atomic<bool> g_firstFrameReady(false);
static std::atomic<unsigned> g_consecutiveFrameTransferFailures(0);

static void SetNativePlayerError(DWORD mediaError, HRESULT hr) {
    g_lastMediaError = mediaError;
    g_lastMediaHr = static_cast<LONG>(hr);
    g_playerState = NativePlayerState::Error;
}

static std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};

    const int chars = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (chars <= 0) return {};

    std::wstring result(static_cast<size_t>(chars), L'\0');
    if (MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()),
        result.data(), chars) != chars) {
        return {};
    }
    return result;
}

// Render Targets for Base Video Frame
static ComPtr<ID3D11Texture2D> g_videoTexture;
static ComPtr<ID3D11ShaderResourceView> g_videoSRV;
static DWORD g_texWidth = 0;
static DWORD g_texHeight = 0;

// CPU Buffer for Robust Software Decode
static std::vector<BYTE> g_cpuFrame;

// Upscaler Shader Pass State
static ComPtr<ID3D11Texture2D> g_upscaleTexture;
static ComPtr<ID3D11ShaderResourceView> g_upscaleSRV;
static ComPtr<ID3D11RenderTargetView> g_upscaleRTV;
static ComPtr<ID3D11PixelShader> g_upscalePS;
static ComPtr<ID3D11VertexShader> g_upscaleVS;
static ComPtr<ID3D11SamplerState> g_upscaleSampler;
static DWORD g_upW = 0;
static DWORD g_upH = 0;


// --------------------------------------------------------------------------------------
// MEDIA ENGINE NOTIFIER CALLBACK
// --------------------------------------------------------------------------------------
class CPlayerNotify : public IMFMediaEngineNotify {
    long m_cRef = 1;
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IMFMediaEngineNotify) || riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG ref = InterlockedDecrement(&m_cRef);
        if (ref == 0) delete this;
        return ref;
    }
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override {
        switch (event) {
        case MF_MEDIA_ENGINE_EVENT_LOADSTART:
            g_playerState = NativePlayerState::Loading;
            break;

        case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
            g_metadataReady = true;
            g_videoFormatChanged = true;
            if (g_playerState != NativePlayerState::Playing)
                g_playerState = NativePlayerState::Ready;
            break;

        case MF_MEDIA_ENGINE_EVENT_LOADEDDATA:
        case MF_MEDIA_ENGINE_EVENT_CANPLAY:
        case MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH:
            if (g_playerState != NativePlayerState::Playing)
                g_playerState = NativePlayerState::Ready;
            break;

        case MF_MEDIA_ENGINE_EVENT_PLAYING:
            g_playerState = NativePlayerState::Playing;
            break;

        case MF_MEDIA_ENGINE_EVENT_PAUSE:
            g_playerState = NativePlayerState::Paused;
            break;

        case MF_MEDIA_ENGINE_EVENT_WAITING:
        case MF_MEDIA_ENGINE_EVENT_STALLED:
        case MF_MEDIA_ENGINE_EVENT_BUFFERINGSTARTED:
            if (g_playerState != NativePlayerState::Error)
                g_playerState = NativePlayerState::Buffering;
            break;

        case MF_MEDIA_ENGINE_EVENT_BUFFERINGENDED:
            if (g_playerState == NativePlayerState::Buffering)
                g_playerState = NativePlayerState::Ready;
            break;

        case MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY:
            g_firstFrameReady = true;
            break;

        case MF_MEDIA_ENGINE_EVENT_ENDED:
            g_playerState = NativePlayerState::Ended;
            break;

        case MF_MEDIA_ENGINE_EVENT_FORMATCHANGE:
            g_videoFormatChanged = true;
            break;

        case MF_MEDIA_ENGINE_EVENT_ERROR:
            // For MF_MEDIA_ENGINE_EVENT_ERROR, param1 is MF_MEDIA_ENGINE_ERR and
            // param2 is the underlying HRESULT (or zero), per Media Foundation.
            SetNativePlayerError(static_cast<DWORD>(param1), static_cast<HRESULT>(param2));
            break;

        default:
            break;
        }
        return S_OK;
    }
};


// --------------------------------------------------------------------------------------
// DEDICATED HIGH-QUALITY SHADER PIPELINE FOR LOSSLESS SCALING
// --------------------------------------------------------------------------------------
void InitUpscaleShaders() {
    if (g_upscaleVS) return;

    // Fullscreen Triangle Generator Vertex Shader
    const char* vsSrc = R"(
        struct VS_OUTPUT {
            float4 pos : SV_POSITION;
            float2 uv : TEXCOORD0;
        };
        VS_OUTPUT main(uint id : SV_VertexID) {
            VS_OUTPUT output;
            output.uv = float2((id << 1) & 2, id & 2);
            output.pos = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
            return output;
        }
    )";

    // Catmull-Rom Fractional Resampling Pixel Shader
    const char* psSrc = R"(
        Texture2D tex : register(t0);
        SamplerState samp : register(s0);

        struct VS_OUTPUT {
            float4 pos : SV_POSITION;
            float2 uv : TEXCOORD0;
        };

        float4 main(VS_OUTPUT input) : SV_Target {
            float2 size;
            tex.GetDimensions(size.x, size.y);
            float2 texel = 1.0 / size;
            
            float2 uv = input.uv * size - 0.5;
            float2 f = frac(uv);
            float2 i = floor(uv);
            
            float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
            float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
            float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
            float2 w3 = f * f * (-0.5 + 0.5 * f);
            
            float2 w12 = w1 + w2;
            float2 offset12 = w2 / w12;
            
            float2 tc0 = (i - 0.5) * texel;
            float2 tc3 = (i + 2.5) * texel;
            float2 tc12 = (i + 0.5 + offset12) * texel;
            
            float4 col0 = tex.SampleLevel(samp, float2(tc12.x, tc0.y), 0) * w12.x +
                          tex.SampleLevel(samp, float2(tc0.x, tc0.y), 0) * w0.x +
                          tex.SampleLevel(samp, float2(tc3.x, tc0.y), 0) * w3.x;
                          
            float4 col12 = tex.SampleLevel(samp, float2(tc12.x, tc12.y), 0) * w12.x +
                           tex.SampleLevel(samp, float2(tc0.x, tc12.y), 0) * w0.x +
                           tex.SampleLevel(samp, float2(tc3.x, tc12.y), 0) * w3.x;
                           
            float4 col3 = tex.SampleLevel(samp, float2(tc12.x, tc3.y), 0) * w12.x +
                          tex.SampleLevel(samp, float2(tc0.x, tc3.y), 0) * w0.x +
                          tex.SampleLevel(samp, float2(tc3.x, tc3.y), 0) * w3.x;
                          
            return col0 * w0.y + col12 * w12.y + col3 * w3.y;
        }
    )";

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    if (SUCCEEDED(D3DCompile(vsSrc, strlen(vsSrc), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errBlob))) {
        g_pd3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_upscaleVS);
    }
    if (SUCCEEDED(D3DCompile(psSrc, strlen(psSrc), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errBlob))) {
        g_pd3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_upscalePS);
    }

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    g_pd3dDevice->CreateSamplerState(&sampDesc, &g_upscaleSampler);
}

void PerformUpscalePass(ID3D11ShaderResourceView* srcSRV, ID3D11RenderTargetView* destRTV, DWORD destW, DWORD destH) {
    if (!g_upscaleVS || !g_upscalePS || !g_upscaleSampler) return;

    // FIX: Force unbind any lingering SRVs to avoid RenderTarget hazards!
    // ImGui leaves slot 0 bound natively, which causes pipeline warnings and wrong textures to persist.
    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    g_pd3dDeviceContext->PSSetShaderResources(0, 1, nullSRV);

    // Secure execution snapshot to prevent pipeline poisoning
    ComPtr<ID3D11RenderTargetView> oldRTV;
    ComPtr<ID3D11DepthStencilView> oldDSV;
    g_pd3dDeviceContext->OMGetRenderTargets(1, &oldRTV, &oldDSV);

    D3D11_VIEWPORT oldVP;
    UINT numVP = 1;
    g_pd3dDeviceContext->RSGetViewports(&numVP, &oldVP);

    // Bind custom scaler context
    g_pd3dDeviceContext->OMSetRenderTargets(1, &destRTV, nullptr);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)destW, (float)destH, 0.0f, 1.0f };
    g_pd3dDeviceContext->RSSetViewports(1, &vp);

    g_pd3dDeviceContext->IASetInputLayout(nullptr);
    g_pd3dDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    g_pd3dDeviceContext->VSSetShader(g_upscaleVS.Get(), nullptr, 0);
    g_pd3dDeviceContext->PSSetShader(g_upscalePS.Get(), nullptr, 0);

    ID3D11ShaderResourceView* srvs[] = { srcSRV };
    g_pd3dDeviceContext->PSSetShaderResources(0, 1, srvs);

    ID3D11SamplerState* samps[] = { g_upscaleSampler.Get() };
    g_pd3dDeviceContext->PSSetSamplers(0, 1, samps);

    g_pd3dDeviceContext->Draw(3, 0);

    // Nullify resource binding to prevent D3D11 write hazards on the next tick
    g_pd3dDeviceContext->PSSetShaderResources(0, 1, nullSRV);

    // Restore execution states properly
    g_pd3dDeviceContext->OMSetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.Get());
    g_pd3dDeviceContext->RSSetViewports(1, &oldVP);
}


// --------------------------------------------------------------------------------------
// IMPLEMENTATION
// --------------------------------------------------------------------------------------

void InitCPlayer(HWND hwnd) {
    if (g_mfInitialized) return;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        SetNativePlayerError(MF_MEDIA_ENGINE_ERR_DECODE, hr);
        return;
    }

    ComPtr<ID3D11Multithread> multiThread;
    if (SUCCEEDED(g_pd3dDevice->QueryInterface(__uuidof(ID3D11Multithread), (void**)&multiThread))) {
        multiThread->SetMultithreadProtected(TRUE);
    }

    hr = MFCreateDXGIDeviceManager(&g_resetToken, &g_dxgiManager);
    if (SUCCEEDED(hr)) {
        g_dxgiManager->ResetDevice(g_pd3dDevice, g_resetToken);
    }

    // Initialize WIC factory for software frame extraction
    if (!g_wicFactory) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wicFactory));
    }

    g_mfInitialized = true;
}

void LoadCPlayer(const std::string& url) {
    StopCPlayer();

    g_metadataReady = false;
    g_videoFormatChanged = false;
    g_firstFrameReady = false;
    g_consecutiveFrameTransferFailures = 0;
    g_lastMediaError = MF_MEDIA_ENGINE_ERR_NOERROR;
    g_lastMediaHr = S_OK;
    g_playerState = NativePlayerState::Loading;

    if (url.empty()) {
        SetNativePlayerError(MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED, E_INVALIDARG);
        return;
    }

    if (g_Settings.hwDecoding == 0 && !g_wicFactory) {
        SetNativePlayerError(MF_MEDIA_ENGINE_ERR_DECODE, E_NOINTERFACE);
        return;
    }

    ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 4);
    if (FAILED(hr) || !attributes) {
        SetNativePlayerError(MF_MEDIA_ENGINE_ERR_DECODE, hr);
        return;
    }

    if (g_Settings.hwDecoding == 1 && g_dxgiManager) {
        hr = attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, g_dxgiManager.Get());
        if (FAILED(hr)) {
            SetNativePlayerError(MF_MEDIA_ENGINE_ERR_DECODE, hr);
            return;
        }
    }

    // No playback HWND/visual is supplied, so this is Media Foundation frame-server mode.
    // The explicit output format is required for TransferVideoFrame().
    hr = attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (FAILED(hr)) {
        SetNativePlayerError(MF_MEDIA_ENGINE_ERR_DECODE, hr);
        return;
    }

    CPlayerNotify* notify = new CPlayerNotify();
    hr = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
    if (FAILED(hr)) {
        notify->Release();
        SetNativePlayerError(MF_MEDIA_ENGINE_ERR_DECODE, hr);
        return;
    }

    ComPtr<IMFMediaEngineClassFactory> factory;
    hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        hr = factory->CreateInstance(0, attributes.Get(), &g_mediaEngine);
    }
    notify->Release();

    if (FAILED(hr) || !g_mediaEngine) {
        SetNativePlayerError(MF_MEDIA_ENGINE_ERR_DECODE, FAILED(hr) ? hr : E_FAIL);
        return;
    }

    const std::wstring wurl = Utf8ToWide(url);
    if (wurl.empty()) {
        SetNativePlayerError(MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED, HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION));
        return;
    }

    BSTR bstrUrl = SysAllocStringLen(wurl.data(), static_cast<UINT>(wurl.size()));
    if (!bstrUrl) {
        SetNativePlayerError(MF_MEDIA_ENGINE_ERR_DECODE, E_OUTOFMEMORY);
        return;
    }

    hr = g_mediaEngine->SetSource(bstrUrl);
    SysFreeString(bstrUrl);
    if (FAILED(hr)) {
        SetNativePlayerError(MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED, hr);
        return;
    }

    SetCPlayerVolume(g_Settings.volume);

    // SetSource starts resource loading asynchronously. Play only when requested;
    // paused starts still load metadata through the Media Engine source pipeline.
    if (g_Settings.isPlaying) {
        hr = g_mediaEngine->Play();
        if (FAILED(hr)) {
            SetNativePlayerError(MF_MEDIA_ENGINE_ERR_DECODE, hr);
        }
    }
}

void RenderCPlayer(ImVec2 pos, ImVec2 size) {
    if (!g_mediaEngine) return;

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(0, 0, 0, 255));

    // Do not attempt to tick textures without reliable metadata coordinates!
    if (g_metadataReady) {

        if (g_videoFormatChanged) {
            g_texWidth = 0;
            g_texHeight = 0;
            g_upW = 0;
            g_upH = 0;
            g_videoFormatChanged = false;
        }

        LONGLONG pts;
        if (g_mediaEngine->OnVideoStreamTick(&pts) == S_OK) {

            DWORD nativeW = 0, nativeH = 0;
            if (g_mediaEngine->HasVideo()) {
                g_mediaEngine->GetNativeVideoSize(&nativeW, &nativeH);
            }

            DWORD targetW = (DWORD)std::max(1.0f, size.x);
            DWORD targetH = (DWORD)std::max(1.0f, size.y);

            bool doUpscale = (g_Settings.engineMode == 2);

            // Render decode at strict source size to prevent engine stretching
            DWORD decodeW = (nativeW > 0) ? nativeW : targetW;
            DWORD decodeH = (nativeH > 0) ? nativeH : targetH;

            if (!doUpscale) {
                // Unscaled mode writes straight to the ImGui canvas boundaries
                decodeW = targetW;
                decodeH = targetH;
            }

            // RESTORE VISUAL DOWNSCALING: Clamp the internal render dimensions to match the UI setting.
            // This mirrors the VLC implementation for consistent memory and visual scaling.
            int maxTargetHeight = decodeH;
            if (g_ActiveProfile.defaultQuality == "1080p") maxTargetHeight = 1080;
            else if (g_ActiveProfile.defaultQuality == "720p") maxTargetHeight = 720;
            else if (g_ActiveProfile.defaultQuality == "360p") maxTargetHeight = 360;
            else if (g_ActiveProfile.defaultQuality == "4k") maxTargetHeight = 2160;

            if (maxTargetHeight > 0 && maxTargetHeight < (int)decodeH) {
                float aspect = (float)decodeW / (float)decodeH;
                decodeH = maxTargetHeight;
                decodeW = (DWORD)(maxTargetHeight * aspect);
                // Ensure width is a multiple of 4 to prevent horizontal scaling shear
                decodeW = (decodeW + 3) & ~3;
            }

            // 1. ALLOCATE CORE DECODE TARGET
            if (g_texWidth != decodeW || g_texHeight != decodeH || !g_videoTexture) {
                g_videoTexture.Reset();
                g_videoSRV.Reset();

                D3D11_TEXTURE2D_DESC desc = {};
                desc.Width = decodeW;
                desc.Height = decodeH;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;

                if (g_Settings.hwDecoding == 1 && g_dxgiManager) {
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                }
                else {
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                }

                if (SUCCEEDED(g_pd3dDevice->CreateTexture2D(&desc, nullptr, &g_videoTexture))) {
                    g_pd3dDevice->CreateShaderResourceView(g_videoTexture.Get(), nullptr, &g_videoSRV);
                    g_texWidth = decodeW;
                    g_texHeight = decodeH;
                }
            }

            // 2. ALLOCATE UPSCALER SHADER TARGET (If Mode = 2)
            DWORD finalW = doUpscale ? (DWORD)(targetW * 1.25f) : targetW;
            DWORD finalH = doUpscale ? (DWORD)(targetH * 1.25f) : targetH;

            if (doUpscale) {
                if (g_upW != finalW || g_upH != finalH || !g_upscaleTexture) {
                    g_upscaleTexture.Reset();
                    g_upscaleSRV.Reset();
                    g_upscaleRTV.Reset();

                    D3D11_TEXTURE2D_DESC udesc = {};
                    udesc.Width = finalW;
                    udesc.Height = finalH;
                    udesc.MipLevels = 1;
                    udesc.ArraySize = 1;
                    udesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    udesc.SampleDesc.Count = 1;
                    udesc.Usage = D3D11_USAGE_DEFAULT;
                    udesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

                    if (SUCCEEDED(g_pd3dDevice->CreateTexture2D(&udesc, nullptr, &g_upscaleTexture))) {
                        g_pd3dDevice->CreateShaderResourceView(g_upscaleTexture.Get(), nullptr, &g_upscaleSRV);
                        g_pd3dDevice->CreateRenderTargetView(g_upscaleTexture.Get(), nullptr, &g_upscaleRTV);
                        g_upW = finalW;
                        g_upH = finalH;
                    }
                }
            }

            // 3. TRANSFER ENGINE PIXELS
            if (g_videoTexture) {
                MFVideoNormalizedRect srcRect = { 0.0f, 0.0f, 1.0f, 1.0f };
                RECT destRect = { 0, 0, (LONG)decodeW, (LONG)decodeH };
                MFARGB border = { 0, 0, 0, 255 };

                ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
                g_pd3dDeviceContext->PSSetShaderResources(0, 1, nullSRV);

                bool frameReady = false; // Prevents overwriting the texture with black on engine failures

                if (g_Settings.hwDecoding == 1 && g_dxgiManager) {
                    ComPtr<IDXGISurface> dxgiSurface;
                    if (SUCCEEDED(g_videoTexture.As(&dxgiSurface))) {
                        HRESULT hrTransfer = g_mediaEngine->TransferVideoFrame(dxgiSurface.Get(), &srcRect, &destRect, &border);
                        if (SUCCEEDED(hrTransfer)) {
                            frameReady = true;
                            g_firstFrameReady = true;
                            g_consecutiveFrameTransferFailures = 0;
                        }
                        else {
                            g_lastMediaHr = static_cast<LONG>(hrTransfer);
                            if (++g_consecutiveFrameTransferFailures >= 3) {
                                SetNativePlayerError(MF_MEDIA_ENGINE_ERR_DECODE, hrTransfer);
                            }
                        }
                    }
                }
                else {
                    // ROBUST SOFTWARE DECODE TRANSFER TO CPU MEMORY USING WIC BITMAP
                    if (g_wicFactory) {
                        UINT currentWicW = 0, currentWicH = 0;
                        if (g_wicBitmap) {
                            g_wicBitmap->GetSize(&currentWicW, &currentWicH);
                        }

                        // Reallocate WIC Bitmap if size mismatches
                        if (!g_wicBitmap || currentWicW != decodeW || currentWicH != decodeH) {
                            g_wicBitmap.Reset();
                            g_wicFactory->CreateBitmap(
                                decodeW,
                                decodeH,
                                GUID_WICPixelFormat32bppPBGRA,
                                WICBitmapCacheOnLoad,
                                &g_wicBitmap
                            );
                        }

                        if (g_wicBitmap) {
                            HRESULT hrTransfer = g_mediaEngine->TransferVideoFrame(g_wicBitmap.Get(), &srcRect, &destRect, &border);

                            if (SUCCEEDED(hrTransfer)) {
                                WICRect rc = { 0, 0, (INT)decodeW, (INT)decodeH };
                                ComPtr<IWICBitmapLock> lock;

                                if (SUCCEEDED(g_wicBitmap->Lock(&rc, WICBitmapLockRead, &lock))) {
                                    BYTE* pData = nullptr;
                                    UINT cbData = 0;

                                    if (SUCCEEDED(lock->GetDataPointer(&cbData, &pData))) {
                                        UINT stride = 0;
                                        if (FAILED(lock->GetStride(&stride)) || stride == 0) {
                                            stride = decodeW * 4;
                                        }

                                        if (g_cpuFrame.size() < cbData) {
                                            g_cpuFrame.resize(cbData);
                                        }

                                        memcpy(g_cpuFrame.data(), pData, cbData);

                                        // Push decoded frame safely from RAM onto the DX11 pipeline.
                                        // WIC row pitch is not guaranteed to equal width * 4.
                                        g_pd3dDeviceContext->UpdateSubresource(
                                            g_videoTexture.Get(),
                                            0,
                                            nullptr,
                                            g_cpuFrame.data(),
                                            stride,
                                            0
                                        );

                                        frameReady = true;
                                        g_firstFrameReady = true;
                                        g_consecutiveFrameTransferFailures = 0;
                                    }
                                }
                            }
                            else {
                                g_lastMediaHr = static_cast<LONG>(hrTransfer);
                                if (++g_consecutiveFrameTransferFailures >= 3) {
                                    SetNativePlayerError(MF_MEDIA_ENGINE_ERR_DECODE, hrTransfer);
                                }
                            }
                        }
                    }
                }

                // 4. TRIGGER TRUE UPSCALER SHADER ONLY IF FRAME TRANSFER WAS SUCCESSFUL
                if (doUpscale && g_videoSRV && g_upscaleRTV && frameReady) {
                    InitUpscaleShaders();
                    PerformUpscalePass(g_videoSRV.Get(), g_upscaleRTV.Get(), finalW, finalH);
                }
            }
        }
    }

    // 5. RENDER CORRECT SURFACE
    ID3D11ShaderResourceView* displaySRV = GetCPlayerShaderResourceView();
    if (displaySRV) {
        drawList->AddImage((ImTextureID)displaySRV, pos, ImVec2(pos.x + size.x, pos.y + size.y));
    }

    const NativePlayerState state = g_playerState.load();
    if (!displaySRV && (state == NativePlayerState::Loading || state == NativePlayerState::Buffering || state == NativePlayerState::Error)) {
        char status[256] = {};
        if (state == NativePlayerState::Error) {
            snprintf(status, sizeof(status), "%s (MF=%lu, HRESULT=0x%08lX)",
                GetCPlayerStatusText(),
                static_cast<unsigned long>(g_lastMediaError.load()),
                static_cast<unsigned long>(g_lastMediaHr.load()));
        }
        else {
            snprintf(status, sizeof(status), "%s", GetCPlayerStatusText());
        }

        const ImVec2 textSize = ImGui::CalcTextSize(status);
        const ImVec2 textPos(
            pos.x + (size.x - textSize.x) * 0.5f,
            pos.y + (size.y - textSize.y) * 0.5f);
        drawList->AddText(textPos, IM_COL32(225, 225, 235, 230), status);
    }
}

void StopCPlayer() {
    if (g_mediaEngine) {
        g_mediaEngine->Shutdown();
        g_mediaEngine.Reset();
    }

    if (g_pd3dDeviceContext) {
        ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
        g_pd3dDeviceContext->PSSetShaderResources(0, 1, nullSRV);
    }

    // Reset async thread states for new links
    g_metadataReady = false;
    g_videoFormatChanged = false;
    g_firstFrameReady = false;
    g_consecutiveFrameTransferFailures = 0;
    g_playerState = NativePlayerState::Idle;
    g_lastMediaError = MF_MEDIA_ENGINE_ERR_NOERROR;
    g_lastMediaHr = S_OK;

    g_videoTexture.Reset();
    g_videoSRV.Reset();
    g_texWidth = 0;
    g_texHeight = 0;

    g_wicBitmap.Reset(); // Wipe the WIC allocation as well

    g_upscaleTexture.Reset();
    g_upscaleSRV.Reset();
    g_upscaleRTV.Reset();
    g_upW = 0;
    g_upH = 0;
}

void SeekCPlayer(float progress) {
    if (!g_mediaEngine) return;
    double duration = g_mediaEngine->GetDuration();
    if (std::isfinite(duration) && duration > 0.0) {
        g_mediaEngine->SetCurrentTime(progress * duration);
    }
}

float GetCPlayerProgress() {
    if (!g_mediaEngine) return -1.0f;
    double duration = g_mediaEngine->GetDuration();
    if (!std::isfinite(duration) || duration <= 0.0) return -1.0f;

    double current = g_mediaEngine->GetCurrentTime();
    return (float)(current / duration);
}

float GetCPlayerDuration() {
    if (!g_mediaEngine) return -1.0f;
    double duration = g_mediaEngine->GetDuration();
    if (!std::isfinite(duration) || duration <= 0.0) return -1.0f;
    return (float)duration;
}

void SetCPlayerVolume(float volume) {
    if (!g_mediaEngine) return;
    float safeVol = std::clamp(volume, 0.0f, 1.0f);
    g_mediaEngine->SetVolume(safeVol);
}

void PauseCPlayer(bool pause) {
    if (!g_mediaEngine) return;
    if (pause) g_mediaEngine->Pause();
    else g_mediaEngine->Play();
}

void SetCPlayerRate(float rate) {
    if (!g_mediaEngine) return;
    const double safeRate = std::clamp(static_cast<double>(rate), 0.25, 4.0);
    g_mediaEngine->SetPlaybackRate(safeRate);
}

int GetCPlayerVideoWidth() {
    if (!g_mediaEngine) return 0;
    DWORD w = 0, h = 0;
    g_mediaEngine->GetNativeVideoSize(&w, &h);
    return (int)w;
}

int GetCPlayerVideoHeight() {
    if (!g_mediaEngine) return 0;
    DWORD w = 0, h = 0;
    g_mediaEngine->GetNativeVideoSize(&w, &h);
    return (int)h;
}

bool CPlayerHasError() {
    return g_playerState.load() == NativePlayerState::Error;
}

bool CPlayerHasUsableMedia() {
    if (!g_mediaEngine || CPlayerHasError()) return false;

    // Do not dismiss the startup screen just because metadata exists.
    // Keep it visible until TransferVideoFrame has produced a real frame.
    return g_firstFrameReady.load();
}

bool CPlayerHasEnded() {
    return g_playerState.load() == NativePlayerState::Ended;
}

DWORD GetCPlayerErrorCode() {
    return g_lastMediaError.load();
}

HRESULT GetCPlayerErrorHRESULT() {
    return static_cast<HRESULT>(g_lastMediaHr.load());
}

const char* GetCPlayerStatusText() {
    switch (g_playerState.load()) {
    case NativePlayerState::Idle:      return "No media loaded";
    case NativePlayerState::Loading:   return "Loading media...";
    case NativePlayerState::Ready:     return "Media ready";
    case NativePlayerState::Playing:   return "Playing";
    case NativePlayerState::Paused:    return "Paused";
    case NativePlayerState::Buffering: return "Buffering...";
    case NativePlayerState::Ended:     return "Playback ended";
    case NativePlayerState::Error:
        switch (g_lastMediaError.load()) {
        case MF_MEDIA_ENGINE_ERR_ABORTED:           return "Playback was aborted";
        case MF_MEDIA_ENGINE_ERR_NETWORK:           return "Network error while loading media";
        case MF_MEDIA_ENGINE_ERR_DECODE:            return "Windows could not decode this media";
        case MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED: return "Media source or codec is not supported by Windows";
        case MF_MEDIA_ENGINE_ERR_ENCRYPTED:         return "Encrypted/protected media is not supported by this player";
        default:                                    return "Media Foundation playback error";
        }
    default: return "Media Foundation player";
    }
}

ID3D11ShaderResourceView* GetCPlayerShaderResourceView() {
    if (g_Settings.engineMode == 2 && g_upscaleSRV) return g_upscaleSRV.Get();
    return g_videoSRV.Get();
}