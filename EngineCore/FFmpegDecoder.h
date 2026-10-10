#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <mutex>
#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "VideoProcessor.h"

class FFmpegDecoder
{
public:
    FFmpegDecoder();
    ~FFmpegDecoder();

    bool Initialize(ID3D11Device* device, VideoColorConverter* converter);
    void Cleanup();

    bool OpenFile(const wchar_t* filePath);
    void CloseFile();

    bool ReadAndRenderFrame(ID3D11Texture2D* targetTexture, int targetWidth, int targetHeight, int64_t* outPtsMs = nullptr);
    bool DecodeNextFrame(int64_t* outPtsMs = nullptr);
    bool RenderCurrentFrame(ID3D11Texture2D* targetTexture, int targetWidth, int targetHeight);
    bool Seek(int64_t timeStampMs);

    bool IsLoaded() const { return m_formatCtx != nullptr && m_videoStreamIdx >= 0; }
    bool HasCachedFrame() const { return m_hasCachedFrame; }
    int64_t GetDurationMs() const { return m_durationMs; }
    int64_t GetCurrentPositionMs() const { return m_currentPosMs; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    double GetFps() const { return m_fps; }

private:
    static AVPixelFormat GetHwFormat(AVCodecContext* ctx, const AVPixelFormat* pix_fmts);
    void UpdateCachedFrameFromAvFrame();

    ID3D11Device* m_d3dDevice = nullptr;
    VideoColorConverter* m_colorConverter = nullptr;

    AVFormatContext* m_formatCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    AVBufferRef* m_hwDeviceCtx = nullptr;
    AVFrame* m_frame = nullptr;
    AVFrame* m_swFrame = nullptr;
    AVPacket* m_packet = nullptr;

    int m_videoStreamIdx = -1;
    int m_width = 0;
    int m_height = 0;
    int64_t m_durationMs = 0;
    int64_t m_currentPosMs = 0;
    double m_fps = 30.0;

    // Retained frame texture (RGBA8) at native video resolution for continuous rendering without black screen
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_cachedFrameRgba;
    bool m_hasCachedFrame = false;

    // CPU upload fallback texture for non-hw decoded media
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_cpuUploadTexture;
    SwsContext* m_swsCtx = nullptr;

    std::recursive_mutex m_lock;
};
