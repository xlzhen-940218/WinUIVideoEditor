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
#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <wrl/client.h>
#include <memory>
#include <mutex>

class D3D11Renderer
{
public:
    D3D11Renderer();
    ~D3D11Renderer();

    bool Initialize(HWND hWnd, int width, int height);
    bool Resize(int width, int height);
    void Cleanup();

    void BeginFrame();
    void EndFrame();
    void Present();

    ID3D11Device* GetDevice() const { return m_device.Get(); }
    ID3D11DeviceContext* GetContext() const { return m_context.Get(); }
    IDXGISwapChain1* GetSwapChain() const { return m_swapChain.Get(); }
    ID3D11Texture2D* GetTargetTexture() const { return m_targetTexture.Get(); }
    ID3D11RenderTargetView* GetTargetRTV() const { return m_targetRtv.Get(); }
    IDXGIAdapter1* GetAdapter() const { return m_adapter.Get(); }

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    bool CreateDeviceAndAdapter();
    bool CreateSwapChain(HWND hWnd, int width, int height);
    bool CreateTargetTexture(int width, int height);

    HWND m_hWnd = nullptr;
    int m_width = 0;
    int m_height = 0;

    Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_backBufferRtv;

    // Intermediate RGBA8 target texture for composite rendering
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_targetTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_targetRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_targetSrv;

    std::mutex m_renderLock;
};
