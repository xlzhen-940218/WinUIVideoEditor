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
#include <vector>
#include <string>
#include <mutex>

#include "EngineCore.h"
#include "SkiaHeaders.h"

class SkiaRenderer
{
public:
    SkiaRenderer();
    ~SkiaRenderer();

    bool Initialize(ID3D11Device* d3dDevice, int width, int height);
    void Cleanup();
    bool Resize(int width, int height);

    bool BeginDraw(ID3D11Texture2D* targetTexture);
    void EndDraw();

    void DrawPrimitive(const PrimitiveElementDesc& desc);

    static void EnumerateFonts(FontEnumerationCallback callback, void* userData);

    sk_canvas_t* GetCanvas() const { return m_skCanvas; }
    sk_surface_t* GetSurface() const { return m_skSurface; }

private:
    void EnsureSurface(int width, int height);
    bool InitShaders();

    ID3D11Device* m_d3dDevice = nullptr;
    ID3D11Texture2D* m_currentTargetTexture = nullptr;

    int m_width = 1280;
    int m_height = 720;
    int m_surfaceWidth = 0;
    int m_surfaceHeight = 0;

    std::vector<uint32_t> m_pixelBuffer;
    sk_surface_t* m_skSurface = nullptr;
    sk_canvas_t* m_skCanvas = nullptr;
    gr_direct_context_t* m_grContext = nullptr;

    // D3D11 GPU staging texture & SRV for zero-copy upload / blending into targetTexture
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_stagingSrv;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendState;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vsQuad;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_psBlend;

    std::mutex m_lock;
};
