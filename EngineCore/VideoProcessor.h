#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <mutex>

class VideoColorConverter
{
public:
    VideoColorConverter();
    ~VideoColorConverter();

    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height);
    void Cleanup();

    void Resize(int width, int height);

    // Blit RGBA8 source texture to RGBA8 destination with aspect ratio preservation (letterbox/pillarbox)
    bool BlitRGBA8(
        ID3D11Texture2D* srcRgba,
        ID3D11Texture2D* dstRgba,
        int srcWidth,
        int srcHeight,
        int dstWidth,
        int dstHeight);

    // Zero-Copy NV12 (YUV) -> RGBA8 on GPU
    bool ConvertNV12ToRGBA8(
        ID3D11Texture2D* nv12Texture, 
        UINT subresource, 
        ID3D11Texture2D* rgba8Texture,
        int srcWidth, 
        int srcHeight, 
        int dstWidth, 
        int dstHeight);

    // Zero-Copy RGBA8 -> NV12 on GPU
    bool ConvertRGBA8ToNV12(
        ID3D11Texture2D* rgba8Texture, 
        ID3D11Texture2D* nv12Texture, 
        UINT subresource,
        int srcWidth, 
        int srcHeight, 
        int dstWidth, 
        int dstHeight);

private:
    bool InitVideoProcessor(int width, int height);
    bool InitShaderFallback();

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;

    // D3D11 Hardware Video Processor
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_videoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_vpEnumerator;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_videoProcessor;

    // Direct3D 11 GPU Pixel Shader fallback for color conversion in VRAM
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vsQuad;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_psNV12ToRGBA;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_psRGBAToNV12;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_psBlitRGBA;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;

    bool m_vpSupported = false;
    bool m_shadersReady = false;
    int m_vpWidth = 0;
    int m_vpHeight = 0;

    std::mutex m_lock;
};
