#include "D3D11Renderer.h"
#include <vector>
#include <iostream>

D3D11Renderer::D3D11Renderer()
{
}

D3D11Renderer::~D3D11Renderer()
{
    Cleanup();
}

void D3D11Renderer::Cleanup()
{
    std::lock_guard<std::mutex> lock(m_renderLock);

    m_targetSrv.Reset();
    m_targetRtv.Reset();
    m_targetTexture.Reset();
    m_backBufferRtv.Reset();
    m_swapChain.Reset();
    m_context.Reset();
    m_device.Reset();
    m_adapter.Reset();
}

bool D3D11Renderer::Initialize(HWND hWnd, int width, int height)
{
    std::lock_guard<std::mutex> lock(m_renderLock);

    m_hWnd = hWnd;
    m_width = width > 0 ? width : 1280;
    m_height = height > 0 ? height : 720;

    if (!CreateDeviceAndAdapter())
    {
        return false;
    }

    if (m_hWnd != nullptr)
    {
        if (!CreateSwapChain(m_hWnd, m_width, m_height))
        {
            return false;
        }
    }

    if (!CreateTargetTexture(m_width, m_height))
    {
        return false;
    }

    return true;
}

bool D3D11Renderer::CreateDeviceAndAdapter()
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        return false;
    }

    // Enumerate best hardware adapter
    Microsoft::WRL::ComPtr<IDXGIAdapter1> bestAdapter;
    SIZE_T maxDedicatedMemory = 0;

    Microsoft::WRL::ComPtr<IDXGIAdapter1> curAdapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &curAdapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc;
        curAdapter->GetDesc1(&desc);

        // Skip software basic render driver
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        if (desc.DedicatedVideoMemory > maxDedicatedMemory || !bestAdapter)
        {
            maxDedicatedMemory = desc.DedicatedVideoMemory;
            bestAdapter = curAdapter;
        }
    }

    m_adapter = bestAdapter;

    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#if defined(_DEBUG)
    // Optional debug flag if SDK layer is available
    // creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
    };
    D3D_FEATURE_LEVEL featureLevel;

    hr = D3D11CreateDevice(
        m_adapter.Get(),
        m_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        creationFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &m_device,
        &featureLevel,
        &m_context);

    if (FAILED(hr))
    {
        // Fallback without D3D_FEATURE_LEVEL_11_1
        hr = D3D11CreateDevice(
            m_adapter.Get(),
            m_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            creationFlags,
            &featureLevels[1],
            ARRAYSIZE(featureLevels) - 1,
            D3D11_SDK_VERSION,
            &m_device,
            &featureLevel,
            &m_context);
    }

    if (FAILED(hr))
    {
        return false;
    }

    // Enable multithread protection
    ID3D11Multithread* multithread = nullptr;
    if (SUCCEEDED(m_device->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&multithread))))
    {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
    }

    return true;
}

bool D3D11Renderer::CreateSwapChain(HWND hWnd, int width, int height)
{
    m_backBufferRtv.Reset();
    m_swapChain.Reset();

    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDevice;
    HRESULT hr = m_device.As(&dxgiDevice);
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory2;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory2));
    if (FAILED(hr)) return false;

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width = width > 0 ? width : 1280;
    scDesc.Height = height > 0 ? height : 720;
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.Stereo = FALSE;
    scDesc.SampleDesc.Count = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2;
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    scDesc.Flags = 0;

    hr = factory2->CreateSwapChainForHwnd(
        m_device.Get(),
        hWnd,
        &scDesc,
        nullptr,
        nullptr,
        &m_swapChain);

    if (FAILED(hr))
    {
        // Try FLIP_SEQUENTIAL fallback for older Windows versions
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hr = factory2->CreateSwapChainForHwnd(
            m_device.Get(),
            hWnd,
            &scDesc,
            nullptr,
            nullptr,
            &m_swapChain);
    }

    if (FAILED(hr)) return false;

    // Create back buffer RTV
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;

    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_backBufferRtv);
    if (FAILED(hr)) return false;

    return true;
}

bool D3D11Renderer::CreateTargetTexture(int width, int height)
{
    m_targetSrv.Reset();
    m_targetRtv.Reset();
    m_targetTexture.Reset();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width > 0 ? width : 1280;
    desc.Height = height > 0 ? height : 720;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_targetTexture);
    if (FAILED(hr)) return false;

    hr = m_device->CreateRenderTargetView(m_targetTexture.Get(), nullptr, &m_targetRtv);
    if (FAILED(hr)) return false;

    hr = m_device->CreateShaderResourceView(m_targetTexture.Get(), nullptr, &m_targetSrv);
    if (FAILED(hr)) return false;

    return true;
}

bool D3D11Renderer::Resize(int width, int height)
{
    std::lock_guard<std::mutex> lock(m_renderLock);

    if (width <= 0 || height <= 0) return false;
    m_width = width;
    m_height = height;

    m_backBufferRtv.Reset();
    if (m_swapChain)
    {
        HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (SUCCEEDED(hr))
        {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
            if (SUCCEEDED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
            {
                m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_backBufferRtv);
            }
        }
    }

    CreateTargetTexture(width, height);
    return true;
}

void D3D11Renderer::BeginFrame()
{
    std::lock_guard<std::mutex> lock(m_renderLock);

    if (!m_context || !m_targetRtv) return;

    // Dark sleek canvas backdrop (RGB: 18, 18, 20)
    const float clearColor[4] = { 0.07f, 0.07f, 0.08f, 1.0f };
    m_context->ClearRenderTargetView(m_targetRtv.Get(), clearColor);

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(m_width);
    vp.Height = static_cast<float>(m_height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    m_context->RSSetViewports(1, &vp);

    ID3D11RenderTargetView* rtvs[1] = { m_targetRtv.Get() };
    m_context->OMSetRenderTargets(1, rtvs, nullptr);
}

void D3D11Renderer::EndFrame()
{
    std::lock_guard<std::mutex> lock(m_renderLock);

    if (!m_context || !m_swapChain || !m_backBufferRtv || !m_targetTexture) return;

    // Unbind render targets and SRVs so targetTexture is not bound during copy
    ID3D11RenderTargetView* nullRtvs[1] = { nullptr };
    m_context->OMSetRenderTargets(1, nullRtvs, nullptr);
    ID3D11ShaderResourceView* nullSrvs[4] = { nullptr, nullptr, nullptr, nullptr };
    m_context->PSSetShaderResources(0, 4, nullSrvs);

    // Blit / copy target texture onto back buffer
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (SUCCEEDED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
    {
        D3D11_TEXTURE2D_DESC bbDesc;
        backBuffer->GetDesc(&bbDesc);

        D3D11_TEXTURE2D_DESC targetDesc;
        m_targetTexture->GetDesc(&targetDesc);

        if (bbDesc.Width == targetDesc.Width && bbDesc.Height == targetDesc.Height)
        {
            m_context->CopyResource(backBuffer.Get(), m_targetTexture.Get());
        }
        else
        {
            // Direct3D 11 Box copy or stretch
            D3D11_BOX box = {};
            box.left = 0;
            box.top = 0;
            box.front = 0;
            box.right = (std::min)(bbDesc.Width, targetDesc.Width);
            box.bottom = (std::min)(bbDesc.Height, targetDesc.Height);
            box.back = 1;
            m_context->CopySubresourceRegion(backBuffer.Get(), 0, 0, 0, 0, m_targetTexture.Get(), 0, &box);
        }
    }
}

void D3D11Renderer::Present()
{
    std::lock_guard<std::mutex> lock(m_renderLock);
    if (m_swapChain)
    {
        m_swapChain->Present(0, 0);
    }
}
