#include "videoengine.hpp"

#pragma message("Ensure libvlc.lib and libvlccore.lib are linked in Visual Studio!")
#include <vlc/vlc.h>
#include <mutex>
#include <algorithm> // For std::swap
#include <malloc.h>  // For _aligned_malloc and _aligned_free
#include <atomic>    // For async playback pause logic

struct VLCContext {
    std::mutex mutex;
    uint8_t* pixels = nullptr;      // ImGui reads from this safely
    uint8_t* backBuffer = nullptr;  // VLC writes to this safely (Double Buffering)
    unsigned width = 0;
    unsigned height = 0;
    unsigned pitch = 0;             // SIMD aligned row pitch
    bool hasNewFrame = false;

    // Helper to safely free AVX aligned memory
    void CleanBuffers() {
        if (pixels) { _aligned_free(pixels); pixels = nullptr; }
        if (backBuffer) { _aligned_free(backBuffer); backBuffer = nullptr; }
    }
};

static libvlc_instance_t* vlc_inst = nullptr;
static libvlc_media_player_t* vlc_mp = nullptr;

static ID3D11Texture2D* vlc_texture = nullptr;
static ID3D11ShaderResourceView* vlc_srv = nullptr;
static VLCContext vlc_ctx;
static unsigned currentTexW = 0, currentTexH = 0;

// Async state to delay pausing until the first frame clears
static std::atomic<bool> g_pauseAfterFirstFrame(false);
static std::atomic<bool> g_gotFirstFrame(false);

static void* lock(void* data, void** p_pixels) {
    VLCContext* ctx = (VLCContext*)data;

    // NO MUTEX LOCK NEEDED HERE!
    // VLC can freely write into the backBuffer while the UI reads from the main pixels.
    if (!ctx->backBuffer && ctx->width > 0 && ctx->height > 0) {
        // ALIGNMENT FIX: Allocate on 32-byte boundaries for AVX/SSE video decoding optimizations
        ctx->backBuffer = (uint8_t*)_aligned_malloc(ctx->pitch * ctx->height, 32);
    }

    *p_pixels = ctx->backBuffer;
    return nullptr;
}

static void unlock(void* data, void* id, void* const* p_pixels) {
    VLCContext* ctx = (VLCContext*)data;

    // VLC is completely done decoding the frame into backBuffer.
    // Lock briefly just to swap the pointers, ensuring the UI thread never hangs!
    std::lock_guard<std::mutex> lock(ctx->mutex);
    std::swap(ctx->pixels, ctx->backBuffer);
    ctx->hasNewFrame = true;
}

static void display(void* data, void* id) {
    // FIX: Delay pause until the first frame is actually presented!
    if (!g_gotFirstFrame.exchange(true) && g_pauseAfterFirstFrame.load() && vlc_mp) {
        libvlc_media_player_set_pause(vlc_mp, 1);
    }
}

static unsigned video_format_cb(void** opaque, char* chroma, unsigned* width, unsigned* height, unsigned* pitches, unsigned* lines) {
    VLCContext* ctx = (VLCContext*)*opaque;

    // FORMAT FIX: RV32 forces VLC to output BGRA instead of RGBA.
    // Windows/DirectX natively use BGRA. This entirely removes the CPU penalty of pixel-swizzling!
    memcpy(chroma, "RV32", 4);

    // RESTORE VISUAL DOWNSCALING: Clamp the internal render dimensions to match the UI setting.
    // This dramatically reduces GPU memory usage and provides the visual blur of lower qualities.
    int targetHeight = *height;
    if (g_ActiveProfile.defaultQuality == "1080p") targetHeight = 1080;
    else if (g_ActiveProfile.defaultQuality == "720p") targetHeight = 720;
    else if (g_ActiveProfile.defaultQuality == "360p") targetHeight = 360;
    else if (g_ActiveProfile.defaultQuality == "4k") targetHeight = 2160;

    if (targetHeight > 0 && targetHeight < (int)*height) {
        float aspect = (float)(*width) / (float)(*height);
        *height = targetHeight;
        *width = (unsigned)(targetHeight * aspect);
        // Ensure width is a multiple of 4 to prevent horizontal scaling shear
        *width = (*width + 3) & ~3;
    }

    // ALIGNMENT FIX: Pitch must be aligned to 32 bytes for optimized vector copies
    unsigned pitch = (*width * 4 + 31) & ~31;

    ctx->mutex.lock();
    ctx->width = *width;
    ctx->height = *height;
    ctx->pitch = pitch;
    ctx->CleanBuffers();
    ctx->mutex.unlock();

    *pitches = pitch;
    *lines = *height;
    return 1;
}

void InitVLCPlayer() {
    if (!vlc_inst) {
        // FIX: Removed "--no-osd" because VLC treats Subtitles (SPU) as an OSD layer!
        // Disabling it completely breaks the text rendering pipeline.
        const char* args[] = { "--drop-late-frames" };
        vlc_inst = libvlc_new(1, args);
    }
}

void LoadVLCPlayer(const std::string& url) {
    StopVLCPlayer();
    if (!vlc_inst || url.empty()) return;

    // libvlc_media_new_path() is for local filesystem paths. Passing an HTTPS
    // stream to it can turn the URL into a bogus local-file URI. Anything with
    // a URI scheme is loaded as a location; plain paths keep the path API.
    const bool isLocation = (url.find("://") != std::string::npos);
    libvlc_media_t* m = isLocation
        ? libvlc_media_new_location(vlc_inst, url.c_str())
        : libvlc_media_new_path(vlc_inst, url.c_str());

    if (!m) return;

    if (g_Settings.hwDecoding == 1) libvlc_media_add_option(m, ":avcodec-hw=any");
    else libvlc_media_add_option(m, ":avcodec-hw=none");

    vlc_mp = libvlc_media_player_new_from_media(m);
    libvlc_media_release(m);

    libvlc_video_set_format_callbacks(vlc_mp, video_format_cb, nullptr);
    libvlc_video_set_callbacks(vlc_mp, lock, unlock, display, &vlc_ctx);

    // FIX: Arm the async pause trap
    g_pauseAfterFirstFrame = !g_Settings.isPlaying;
    g_gotFirstFrame = false;

    libvlc_media_player_play(vlc_mp);
    SetVLCVolume(g_Settings.volume);
}

void RenderVLCPlayer(ImVec2 pos, ImVec2 size) {
    if (vlc_ctx.hasNewFrame && g_pd3dDeviceContext) {

        // FIX: try_lock() completely decouples the UI framerate from the VLC Decoder!
        if (vlc_ctx.mutex.try_lock()) {
            if ((currentTexW != vlc_ctx.width || currentTexH != vlc_ctx.height || !vlc_texture) && vlc_ctx.width > 0 && vlc_ctx.height > 0) {
                if (vlc_srv) { vlc_srv->Release(); vlc_srv = nullptr; }
                if (vlc_texture) { vlc_texture->Release(); vlc_texture = nullptr; }

                D3D11_TEXTURE2D_DESC desc = {};
                desc.Width = vlc_ctx.width;
                desc.Height = vlc_ctx.height;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                // MATCH FIX: B8G8R8A8 perfectly matches VLC's RV32 output (Zero-cost GPU translation)
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

                if (SUCCEEDED(g_pd3dDevice->CreateTexture2D(&desc, nullptr, &vlc_texture))) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.Format = desc.Format;
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = 1;
                    g_pd3dDevice->CreateShaderResourceView(vlc_texture, &srvDesc, &vlc_srv);
                }
                currentTexW = vlc_ctx.width;
                currentTexH = vlc_ctx.height;
            }

            if (vlc_texture && vlc_ctx.pixels) {
                // FIX Driver crash: Map might fail if resource is still bound. Unbind just in case.
                ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
                g_pd3dDeviceContext->PSSetShaderResources(0, 1, nullSRV);

                D3D11_MAPPED_SUBRESOURCE mapped;
                if (g_pd3dDeviceContext->Map(vlc_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped) == S_OK) {

                    // FIX: Safer block copy utilizing the 32-byte aligned SIMD pitch
                    if (mapped.RowPitch == vlc_ctx.pitch) {
                        memcpy(mapped.pData, vlc_ctx.pixels, vlc_ctx.pitch * vlc_ctx.height);
                    }
                    else {
                        for (unsigned int y = 0; y < vlc_ctx.height; y++) {
                            memcpy((uint8_t*)mapped.pData + y * mapped.RowPitch,
                                vlc_ctx.pixels + y * vlc_ctx.pitch,
                                std::min((unsigned)mapped.RowPitch, vlc_ctx.pitch));
                        }
                    }
                    g_pd3dDeviceContext->Unmap(vlc_texture, 0);
                }
            }
            vlc_ctx.hasNewFrame = false;
            vlc_ctx.mutex.unlock();
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (vlc_srv) {
        dl->AddImage((ImTextureID)vlc_srv, pos, ImVec2(pos.x + size.x, pos.y + size.y));
    }
    else {
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(20, 15, 10, 255));
    }
}

void StopVLCPlayer() {
    if (vlc_mp) {
        libvlc_media_player_stop(vlc_mp);
        libvlc_media_player_release(vlc_mp);
        vlc_mp = nullptr;
    }

    // Safely unbind from the pipeline before destroying resources
    if (g_pd3dDeviceContext) {
        ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
        g_pd3dDeviceContext->PSSetShaderResources(0, 1, nullSRV);
    }

    // Free the DX11 textures to prevent VRAM leaks when swapping to CPlayer
    if (vlc_srv) { vlc_srv->Release(); vlc_srv = nullptr; }
    if (vlc_texture) { vlc_texture->Release(); vlc_texture = nullptr; }
    currentTexW = 0;
    currentTexH = 0;

    // Clean up memory buffer immediately when stopped to prevent lingering memory consumption
    std::lock_guard<std::mutex> lock(vlc_ctx.mutex);
    vlc_ctx.CleanBuffers();
    vlc_ctx.width = 0;
    vlc_ctx.height = 0;
    vlc_ctx.hasNewFrame = false; // FIX: Wipe state cleanly so reloading won't flash the old frame
}

void SeekVLCPlayer(float progress) {
    if (vlc_mp) libvlc_media_player_set_position(vlc_mp, progress);
}

float GetVLCPlayerProgress() {
    if (vlc_mp) return libvlc_media_player_get_position(vlc_mp);
    return -1.0f;
}

float GetVLCDuration() {
    if (vlc_mp) {
        libvlc_time_t len = libvlc_media_player_get_length(vlc_mp);
        if (len > 0) return (float)(len / 1000.0); // Return seconds
    }
    return -1.0f;
}

void SetVLCVolume(float volume) {
    if (vlc_mp) libvlc_audio_set_volume(vlc_mp, (int)(volume * 100.0f));
}

void PauseVLCPlayer(bool pause) {
    if (vlc_mp) libvlc_media_player_set_pause(vlc_mp, pause ? 1 : 0);
}

void SetVLCRate(float rate) {
    if (!vlc_mp) return;
    const float safeRate = std::clamp(rate, 0.25f, 4.0f);
    libvlc_media_player_set_rate(vlc_mp, safeRate);
}

bool VLCPlayerHasEnded() {
    return vlc_mp && libvlc_media_player_get_state(vlc_mp) == libvlc_Ended;
}

int GetVLCVideoWidth() {
    if (!vlc_mp) return 0;
    unsigned width = 0, height = 0;
    if (libvlc_video_get_size(vlc_mp, 0, &width, &height) == 0) return width;
    return 0;
}

int GetVLCVideoHeight() {
    if (!vlc_mp) return 0;
    unsigned width = 0, height = 0;
    if (libvlc_video_get_size(vlc_mp, 0, &width, &height) == 0) return height;
    return 0;
}

std::vector<SubTrack> GetVLCSpuTracks() {
    std::vector<SubTrack> tracks;
    if (!vlc_mp) return tracks;

    libvlc_track_description_t* spu = libvlc_video_get_spu_description(vlc_mp);
    libvlc_track_description_t* track = spu;
    while (track) {
        if (track->psz_name) tracks.push_back({ track->i_id, track->psz_name });
        track = track->p_next;
    }
    libvlc_track_description_list_release(spu);
    return tracks;
}

void SetVLCSpuTrack(int id) {
    if (vlc_mp) libvlc_video_set_spu(vlc_mp, id);
}

ID3D11ShaderResourceView* GetVLCShaderResourceView() {
    return vlc_srv;
}