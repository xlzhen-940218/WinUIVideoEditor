#include "VideoProcessor.h"
#include <d3dcompiler.h>
#include <iostream>
#include <algorithm>

#pragma comment(lib, "d3dcompiler.lib")

VideoColorConverter::VideoColorConverter()
{
}

VideoColorConverter::~VideoColorConverter()
{
    Cleanup();
}

void VideoColorConverter::Cleanup()
{
    std::lock_guard<std::mutex> lock(m_lock);

    m_sampler.Reset();
    m_psRGBAToNV12.Reset();
    m_psNV12ToRGBA.Reset();
    m_vsQuad.Reset();
    m_videoProcessor.Reset();
    m_vpEnumerator.Reset();
    m_videoContext.Reset();
    m_videoDevice.Reset();
    m_context.Reset();
    m_device.Reset();

    m_vpSupported = false;
    m_shadersReady = false;
}

bool VideoColorConverter::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height)
{
    std::lock_guard<std::mutex> lock(m_lock);

    m_device = device;
    m_context = context;
    m_vpWidth = width > 0 ? width : 1280;
    m_vpHeight = height > 0 ? height : 720;

    InitVideoProcessor(m_vpWidth, m_vpHeight);
    InitShaderFallback();

    return m_vpSupported || m_shadersReady;
}

bool VideoColorConverter::InitVideoProcessor(int width, int height)
{
    if (!m_device || !m_context) return false;

    HRESULT hr = m_device.As(&m_videoDevice);
    if (FAILED(hr)) return false;

    hr = m_context.As(&m_videoContext);
    if (FAILED(hr)) return false;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = {};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputWidth = width;
    desc.InputHeight = height;
    desc.OutputWidth = width;
    desc.OutputHeight = height;
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = m_videoDevice->CreateVideoProcessorEnumerator(&desc, &m_vpEnumerator);
    if (FAILED(hr)) return false;

    UINT flags = 0;
    hr = m_vpEnumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &flags);
    if (FAILED(hr) || !(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))
    {
        return false;
    }

    hr = m_videoDevice->CreateVideoProcessor(m_vpEnumerator.Get(), 0, &m_videoProcessor);
    if (FAILED(hr)) return false;

    m_vpSupported = true;
    return true;
}

bool VideoColorConverter::InitShaderFallback()
{
    if (!m_device) return false;

    // Full screen triangle vertex shader
    const char* vsSource = R"(
        struct VS_OUTPUT {
            float4 pos : SV_POSITION;
            float2 uv  : TEXCOORD0;
        };
        VS_OUTPUT main(uint id : SV_VertexID) {
            VS_OUTPUT output;
            output.uv = float2((id << 1) & 2, id & 2);
            output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
            return output;
        }
    )";

    // NV12 to RGBA8 pixel shader (BT.709 color space conversion in VRAM)
    const char* psNV12Source = R"(
        Texture2D<float>  texY  : register(t0);
        Texture2D<float2> texUV : register(t1);
        SamplerState samp : register(s0);

        struct VS_OUTPUT {
            float4 pos : SV_POSITION;
            float2 uv  : TEXCOORD0;
        };

        float4 main(VS_OUTPUT input) : SV_TARGET {
            float y = texY.Sample(samp, input.uv);
            float2 uv = texUV.Sample(samp, input.uv);
            float u = uv.x - 0.5f;
            float v = uv.y - 0.5f;

            // BT.709 standard matrix
            float r = y + 1.5748f * v;
            float g = y - 0.1873f * u - 0.4681f * v;
            float b = y + 1.8556f * u;

            return float4(saturate(r), saturate(g), saturate(b), 1.0f);
        }
    )";

    // RGBA to NV12 pixel shader (for hardware export)
    const char* psRGBAToNV12Source = R"(
        Texture2D<float4> texRGBA : register(t0);
        SamplerState samp : register(s0);

        struct VS_OUTPUT {
            float4 pos : SV_POSITION;
            float2 uv  : TEXCOORD0;
        };

        float4 main(VS_OUTPUT input) : SV_TARGET {
            float4 rgba = texRGBA.Sample(samp, input.uv);
            // BT.709 RGB to YUV
            float y = 0.2126f * rgba.r + 0.7152f * rgba.g + 0.0722f * rgba.b;
            return float4(y, y, y, 1.0f);
        }
    )";

    // RGBA to RGBA pixel shader (for bilinear blitting with letterbox)
    const char* psBlitRGBASource = R"(
        Texture2D<float4> texRGBA : register(t0);
        SamplerState samp : register(s0);

        struct VS_OUTPUT {
            float4 pos : SV_POSITION;
            float2 uv  : TEXCOORD0;
        };

        float4 main(VS_OUTPUT input) : SV_TARGET {
            return texRGBA.Sample(samp, input.uv);
        }
    )";

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psNV12Blob, psRGBABlob, psBlitBlob, errBlob;
    HRESULT hr = D3DCompile(vsSource, strlen(vsSource), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) return false;

    hr = D3DCompile(psNV12Source, strlen(psNV12Source), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psNV12Blob, &errBlob);
    if (FAILED(hr)) return false;

    hr = D3DCompile(psRGBAToNV12Source, strlen(psRGBAToNV12Source), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psRGBABlob, &errBlob);
    if (FAILED(hr)) return false;

    hr = D3DCompile(psBlitRGBASource, strlen(psBlitRGBASource), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlitBlob, &errBlob);
    if (FAILED(hr)) return false;

    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vsQuad);
    if (FAILED(hr)) return false;

    hr = m_device->CreatePixelShader(psNV12Blob->GetBufferPointer(), psNV12Blob->GetBufferSize(), nullptr, &m_psNV12ToRGBA);
    if (FAILED(hr)) return false;

    hr = m_device->CreatePixelShader(psRGBABlob->GetBufferPointer(), psRGBABlob->GetBufferSize(), nullptr, &m_psRGBAToNV12);
    if (FAILED(hr)) return false;

    hr = m_device->CreatePixelShader(psBlitBlob->GetBufferPointer(), psBlitBlob->GetBufferSize(), nullptr, &m_psBlitRGBA);
    if (FAILED(hr)) return false;

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = m_device->CreateSamplerState(&sampDesc, &m_sampler);
    if (FAILED(hr)) return false;

    m_shadersReady = true;
    return true;
}

void VideoColorConverter::Resize(int width, int height)
{
    std::lock_guard<std::mutex> lock(m_lock);
    m_vpWidth = width > 0 ? width : 1280;
    m_vpHeight = height > 0 ? height : 720;
    m_videoProcessor.Reset();
    m_vpEnumerator.Reset();
    InitVideoProcessor(m_vpWidth, m_vpHeight);
}

bool VideoColorConverter::BlitRGBA8(
    ID3D11Texture2D* srcRgba, 
    ID3D11Texture2D* dstRgba, 
    int srcWidth, 
    int srcHeight, 
    int dstWidth, 
    int dstHeight)
{
    std::lock_guard<std::mutex> lock(m_lock);

    if (!m_device || !m_context || !srcRgba || !dstRgba) return false;
    if (!m_shadersReady || !m_vsQuad || !m_psBlitRGBA) return false;

    // Calculate aspect ratio fit (letterbox/pillarbox)
    int fitW = dstWidth;
    int fitH = dstHeight;
    int offsetX = 0;
    int offsetY = 0;

    if (srcWidth > 0 && srcHeight > 0 && dstWidth > 0 && dstHeight > 0)
    {
        double srcAspect = static_cast<double>(srcWidth) / static_cast<double>(srcHeight);
        double dstAspect = static_cast<double>(dstWidth) / static_cast<double>(dstHeight);

        if (srcAspect > dstAspect)
        {
            fitW = dstWidth;
            fitH = (std::max)(1, static_cast<int>(std::round(dstWidth / srcAspect)));
            offsetY = (dstHeight - fitH) / 2;
        }
        else
        {
            fitH = dstHeight;
            fitW = (std::max)(1, static_cast<int>(std::round(dstHeight * srcAspect)));
            offsetX = (dstWidth - fitW) / 2;
        }
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = m_device->CreateShaderResourceView(srcRgba, nullptr, &srv);
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    hr = m_device->CreateRenderTargetView(dstRgba, nullptr, &rtv);
    if (FAILED(hr)) return false;

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = static_cast<float>(offsetX);
    vp.TopLeftY = static_cast<float>(offsetY);
    vp.Width = static_cast<float>(fitW);
    vp.Height = static_cast<float>(fitH);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);

    ID3D11RenderTargetView* rtvs[1] = { rtv.Get() };
    m_context->OMSetRenderTargets(1, rtvs, nullptr);

    ID3D11ShaderResourceView* srvs[1] = { srv.Get() };
    m_context->PSSetShaderResources(0, 1, srvs);

    ID3D11SamplerState* samplers[1] = { m_sampler.Get() };
    m_context->PSSetSamplers(0, 1, samplers);

    m_context->VSSetShader(m_vsQuad.Get(), nullptr, 0);
    m_context->PSSetShader(m_psBlitRGBA.Get(), nullptr, 0);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrvs[1] = { nullptr };
    m_context->PSSetShaderResources(0, 1, nullSrvs);

    return true;
}

bool VideoColorConverter::ConvertNV12ToRGBA8(
    ID3D11Texture2D* nv12Texture, 
    UINT subresource, 
    ID3D11Texture2D* rgba8Texture,
    int srcWidth, 
    int srcHeight, 
    int dstWidth, 
    int dstHeight)
{
    std::lock_guard<std::mutex> lock(m_lock);

    if (!m_device || !m_context || !nv12Texture || !rgba8Texture) return false;

    // Calculate aspect ratio fit (letterbox/pillarbox)
    int fitW = dstWidth;
    int fitH = dstHeight;
    int offsetX = 0;
    int offsetY = 0;

    if (srcWidth > 0 && srcHeight > 0 && dstWidth > 0 && dstHeight > 0)
    {
        double srcAspect = static_cast<double>(srcWidth) / static_cast<double>(srcHeight);
        double dstAspect = static_cast<double>(dstWidth) / static_cast<double>(dstHeight);

        if (srcAspect > dstAspect)
        {
            fitW = dstWidth;
            fitH = (std::max)(1, static_cast<int>(std::round(dstWidth / srcAspect)));
            offsetY = (dstHeight - fitH) / 2;
        }
        else
        {
            fitH = dstHeight;
            fitW = (std::max)(1, static_cast<int>(std::round(dstHeight * srcAspect)));
            offsetX = (dstWidth - fitW) / 2;
        }
    }

    // First attempt: Hardware Video Processor
    if (m_vpSupported && m_videoProcessor && m_videoContext && m_vpEnumerator)
    {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc = {};
        inDesc.FourCC = 0;
        inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inDesc.Texture2D.MipSlice = 0;
        inDesc.Texture2D.ArraySlice = subresource;

        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView;
        HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(nv12Texture, m_vpEnumerator.Get(), &inDesc, &inView);

        if (SUCCEEDED(hr))
        {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc = {};
            outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            outDesc.Texture2D.MipSlice = 0;

            Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outView;
            hr = m_videoDevice->CreateVideoProcessorOutputView(rgba8Texture, m_vpEnumerator.Get(), &outDesc, &outView);

            if (SUCCEEDED(hr))
            {
                D3D11_VIDEO_PROCESSOR_STREAM stream = {};
                stream.Enable = TRUE;
                stream.OutputIndex = 0;
                stream.InputFrameOrField = 0;
                stream.pInputSurface = inView.Get();

                RECT srcRect = { 0, 0, srcWidth, srcHeight };
                RECT dstRect = { offsetX, offsetY, offsetX + fitW, offsetY + fitH };
                RECT targetRect = { 0, 0, dstWidth, dstHeight };

                m_videoContext->VideoProcessorSetStreamSourceRect(m_videoProcessor.Get(), 0, TRUE, &srcRect);
                m_videoContext->VideoProcessorSetStreamDestRect(m_videoProcessor.Get(), 0, TRUE, &dstRect);
                m_videoContext->VideoProcessorSetOutputTargetRect(m_videoProcessor.Get(), TRUE, &targetRect);

                hr = m_videoContext->VideoProcessorBlt(m_videoProcessor.Get(), outView.Get(), 0, 1, &stream);
                if (SUCCEEDED(hr))
                {
                    return true;
                }
            }
        }
    }

    // Direct3D 11 Shader Path (Zero-copy in GPU VRAM)
    if (m_shadersReady && m_vsQuad && m_psNV12ToRGBA)
    {
        D3D11_TEXTURE2D_DESC texDesc = {};
        nv12Texture->GetDesc(&texDesc);

        D3D11_SHADER_RESOURCE_VIEW_DESC srvYDesc = {};
        srvYDesc.Format = DXGI_FORMAT_R8_UNORM;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvUVDesc = {};
        srvUVDesc.Format = DXGI_FORMAT_R8G8_UNORM;

        if (texDesc.ArraySize > 1)
        {
            srvYDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srvYDesc.Texture2DArray.MipLevels = 1;
            srvYDesc.Texture2DArray.MostDetailedMip = 0;
            srvYDesc.Texture2DArray.FirstArraySlice = subresource;
            srvYDesc.Texture2DArray.ArraySize = 1;

            srvUVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srvUVDesc.Texture2DArray.MipLevels = 1;
            srvUVDesc.Texture2DArray.MostDetailedMip = 0;
            srvUVDesc.Texture2DArray.FirstArraySlice = subresource;
            srvUVDesc.Texture2DArray.ArraySize = 1;
        }
        else
        {
            srvYDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvYDesc.Texture2D.MipLevels = 1;
            srvYDesc.Texture2D.MostDetailedMip = 0;

            srvUVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvUVDesc.Texture2D.MipLevels = 1;
            srvUVDesc.Texture2D.MostDetailedMip = 0;
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvY, srvUV;
        HRESULT hrY = m_device->CreateShaderResourceView(nv12Texture, &srvYDesc, &srvY);
        HRESULT hrUV = m_device->CreateShaderResourceView(nv12Texture, &srvUVDesc, &srvUV);

        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        HRESULT hrRtv = m_device->CreateRenderTargetView(rgba8Texture, nullptr, &rtv);

        if (SUCCEEDED(hrY) && SUCCEEDED(hrUV) && SUCCEEDED(hrRtv))
        {
            D3D11_VIEWPORT vp = {};
            vp.TopLeftX = static_cast<float>(offsetX);
            vp.TopLeftY = static_cast<float>(offsetY);
            vp.Width = static_cast<float>(fitW);
            vp.Height = static_cast<float>(fitH);
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            m_context->RSSetViewports(1, &vp);

            ID3D11RenderTargetView* rtvs[1] = { rtv.Get() };
            m_context->OMSetRenderTargets(1, rtvs, nullptr);

            ID3D11ShaderResourceView* srvs[2] = { srvY.Get(), srvUV.Get() };
            m_context->PSSetShaderResources(0, 2, srvs);

            ID3D11SamplerState* samplers[1] = { m_sampler.Get() };
            m_context->PSSetSamplers(0, 1, samplers);

            m_context->VSSetShader(m_vsQuad.Get(), nullptr, 0);
            m_context->PSSetShader(m_psNV12ToRGBA.Get(), nullptr, 0);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            m_context->Draw(3, 0);

            // Unbind
            ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
            m_context->PSSetShaderResources(0, 2, nullSrvs);

            return true;
        }
    }

    return false;
}

bool VideoColorConverter::ConvertRGBA8ToNV12(
    ID3D11Texture2D* rgba8Texture, 
    ID3D11Texture2D* nv12Texture, 
    UINT subresource,
    int srcWidth, 
    int srcHeight, 
    int dstWidth, 
    int dstHeight)
{
    std::lock_guard<std::mutex> lock(m_lock);

    if (!m_device || !m_context || !rgba8Texture || !nv12Texture) return false;

    // Use VideoProcessor if available
    if (m_vpSupported && m_videoProcessor && m_videoContext && m_vpEnumerator)
    {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc = {};
        inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inDesc.Texture2D.MipSlice = 0;

        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView;
        HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(rgba8Texture, m_vpEnumerator.Get(), &inDesc, &inView);

        if (SUCCEEDED(hr))
        {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc = {};
            outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            outDesc.Texture2D.MipSlice = 0;

            Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outView;
            hr = m_videoDevice->CreateVideoProcessorOutputView(nv12Texture, m_vpEnumerator.Get(), &outDesc, &outView);

            if (SUCCEEDED(hr))
            {
                D3D11_VIDEO_PROCESSOR_STREAM stream = {};
                stream.Enable = TRUE;
                stream.OutputIndex = 0;
                stream.InputFrameOrField = 0;
                stream.pInputSurface = inView.Get();

                RECT srcRect = { 0, 0, srcWidth, srcHeight };
                RECT dstRect = { 0, 0, dstWidth, dstHeight };

                m_videoContext->VideoProcessorSetStreamSourceRect(m_videoProcessor.Get(), 0, TRUE, &srcRect);
                m_videoContext->VideoProcessorSetStreamDestRect(m_videoProcessor.Get(), 0, TRUE, &dstRect);
                m_videoContext->VideoProcessorSetOutputTargetRect(m_videoProcessor.Get(), TRUE, &dstRect);

                hr = m_videoContext->VideoProcessorBlt(m_videoProcessor.Get(), outView.Get(), 0, 1, &stream);
                if (SUCCEEDED(hr))
                {
                    return true;
                }
            }
        }
    }

    // Direct Copy fallback if format matches or staging blit
    m_context->CopyResource(nv12Texture, rgba8Texture);
    return true;
}
