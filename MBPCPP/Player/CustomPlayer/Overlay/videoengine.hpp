#pragma once
#include "main.hpp"

// Subtitle extraction struct
struct SubTrack {
    int id;
    std::string name;
};

// Windows-native Media Foundation frame-server player
void InitCPlayer(HWND hwnd);
void LoadCPlayer(const std::string& url);
void RenderCPlayer(ImVec2 pos, ImVec2 size);
void StopCPlayer();
void SeekCPlayer(float progress);
float GetCPlayerProgress();
float GetCPlayerDuration();
void SetCPlayerVolume(float volume);
void PauseCPlayer(bool pause);
int GetCPlayerVideoWidth();
int GetCPlayerVideoHeight();
bool CPlayerHasError();
bool CPlayerHasUsableMedia();
bool CPlayerHasEnded();
void SetCPlayerRate(float rate);
DWORD GetCPlayerErrorCode();
HRESULT GetCPlayerErrorHRESULT();
const char* GetCPlayerStatusText();

// LibVLC Hardware Memory Player
void InitVLCPlayer();
void LoadVLCPlayer(const std::string& url);
void RenderVLCPlayer(ImVec2 pos, ImVec2 size);
void StopVLCPlayer();
void SeekVLCPlayer(float progress);
float GetVLCPlayerProgress();
float GetVLCDuration();
void SetVLCVolume(float volume);
void PauseVLCPlayer(bool pause);
bool VLCPlayerHasEnded();
void SetVLCRate(float rate);

// Hardware specific metadata accessors
int GetVLCVideoWidth();
int GetVLCVideoHeight();
std::vector<SubTrack> GetVLCSpuTracks();
void SetVLCSpuTrack(int id);

// Export active DirectX 11 Video Textures so DynHTML can perform Backdrop-Filter Blurring!
ID3D11ShaderResourceView* GetVLCShaderResourceView();
ID3D11ShaderResourceView* GetCPlayerShaderResourceView();

// VideoEngine specific helpers
void SaveCurrentEpisodeSettings();
void LerpFade(float& current, float target, float speed);