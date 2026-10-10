#include "SkiaRenderer.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dwrite.h>
#include <iostream>
#include <algorithm>

#pragma comment(lib, "libSkiaSharp.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d3dcompiler.lib")

SkiaRenderer::SkiaRenderer()
{
}

SkiaRenderer::~SkiaRenderer()
{
    Cleanup();
}

void SkiaRenderer::Cleanup()
{
    std::lock_guard<std::mutex> lock(m_lock);

    if (m_skSurface)
    {
        sk_surface_unref(m_skSurface);
        m_skSurface = nullptr;
        m_skCanvas = nullptr;
    }

    m_psBlend.Reset();
    m_vsQuad.Reset();
    m_sampler.Reset();
    m_blendState.Reset();
    m_stagingSrv.Reset();
    m_stagingTexture.Reset();
    m_pixelBuffer.clear();
    m_currentTargetTexture = nullptr;
    m_d3dDevice = nullptr;
}

bool SkiaRenderer::InitShaders()
{
    if (!m_d3dDevice) return false;

    // Full screen triangle vertex shader (3 vertices generated via SV_VertexID)
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

    // Premultiplied alpha composite pixel shader
    const char* psSource = R"(
        Texture2D<float4> texOverlay : register(t0);
        SamplerState samp : register(s0);

        struct VS_OUTPUT {
            float4 pos : SV_POSITION;
            float2 uv  : TEXCOORD0;
        };

        float4 main(VS_OUTPUT input) : SV_TARGET {
            return texOverlay.Sample(samp, input.uv);
        }
    )";

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    HRESULT hr = D3DCompile(vsSource, strlen(vsSource), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) return false;

    hr = D3DCompile(psSource, strlen(psSource), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) return false;

    hr = m_d3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vsQuad);
    if (FAILED(hr)) return false;

    hr = m_d3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_psBlend);
    if (FAILED(hr)) return false;

    // Sampler
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = m_d3dDevice->CreateSamplerState(&sampDesc, &m_sampler);
    if (FAILED(hr)) return false;

    // Blend state: Premultiplied alpha blending
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = m_d3dDevice->CreateBlendState(&blendDesc, &m_blendState);
    return SUCCEEDED(hr);
}

bool SkiaRenderer::Initialize(ID3D11Device* d3dDevice, int width, int height)
{
    std::lock_guard<std::mutex> lock(m_lock);

    m_d3dDevice = d3dDevice;
    m_width = width > 0 ? width : 1280;
    m_height = height > 0 ? height : 720;

    EnsureSurface(m_width, m_height);

    // Create D3D11 dynamic staging texture for fast GPU updates
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = m_width;
    desc.Height = m_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = m_d3dDevice->CreateTexture2D(&desc, nullptr, &m_stagingTexture);
    if (FAILED(hr)) return false;

    hr = m_d3dDevice->CreateShaderResourceView(m_stagingTexture.Get(), nullptr, &m_stagingSrv);
    if (FAILED(hr)) return false;

    return InitShaders();
}

void SkiaRenderer::EnsureSurface(int width, int height)
{
    if (m_skSurface && m_surfaceWidth == width && m_surfaceHeight == height)
    {
        return;
    }

    if (m_skSurface)
    {
        sk_surface_unref(m_skSurface);
        m_skSurface = nullptr;
        m_skCanvas = nullptr;
    }

    m_surfaceWidth = width;
    m_surfaceHeight = height;
    m_width = width;
    m_height = height;
    m_pixelBuffer.assign(static_cast<size_t>(m_width) * m_height, 0);

    sk_imageinfo_t info = {};
    info.colorspace = nullptr;
    info.width = m_width;
    info.height = m_height;
    info.colorType = SK_COLORTYPE_BGRA_8888;
    info.alphaType = SK_ALPHATYPE_PREMUL;

    m_skSurface = sk_surface_new_raster_direct(
        &info, 
        m_pixelBuffer.data(), 
        static_cast<size_t>(m_width) * sizeof(uint32_t), 
        nullptr, nullptr, nullptr);

    if (m_skSurface)
    {
        m_skCanvas = sk_surface_get_canvas(m_skSurface);
    }
}

bool SkiaRenderer::Resize(int width, int height)
{
    std::lock_guard<std::mutex> lock(m_lock);

    if (width <= 0 || height <= 0 || !m_d3dDevice) return false;

    // Ensure surface and pixel buffer are resized to the new dimensions
    EnsureSurface(width, height);

    m_stagingSrv.Reset();
    m_stagingTexture.Reset();
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = m_width;
    desc.Height = m_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = m_d3dDevice->CreateTexture2D(&desc, nullptr, &m_stagingTexture);
    if (FAILED(hr)) return false;

    hr = m_d3dDevice->CreateShaderResourceView(m_stagingTexture.Get(), nullptr, &m_stagingSrv);
    return SUCCEEDED(hr);
}

bool SkiaRenderer::BeginDraw(ID3D11Texture2D* targetTexture)
{
    std::lock_guard<std::mutex> lock(m_lock);

    if (!m_skCanvas || !targetTexture) return false;

    m_currentTargetTexture = targetTexture;

    // Clear Skia surface with transparent backdrop for clean compositing
    sk_canvas_clear(m_skCanvas, 0x00000000);
    return true;
}

void SkiaRenderer::DrawPrimitive(const PrimitiveElementDesc& desc)
{
    std::lock_guard<std::mutex> lock(m_lock);

    if (!m_skCanvas) return;

    if (desc.ElementType == 0) // Text
    {
        if (desc.ContentText[0] == L'\0') return;

        // Convert wchar_t strings to UTF-8
        int textLen = WideCharToMultiByte(CP_UTF8, 0, desc.ContentText, -1, nullptr, 0, nullptr, nullptr);
        std::string utf8Text(textLen > 0 ? textLen - 1 : 0, '\0');
        if (textLen > 0)
        {
            WideCharToMultiByte(CP_UTF8, 0, desc.ContentText, -1, &utf8Text[0], textLen, nullptr, nullptr);
        }

        std::string utf8Family = "Segoe UI";
        if (desc.FontFamily[0] != L'\0')
        {
            int famLen = WideCharToMultiByte(CP_UTF8, 0, desc.FontFamily, -1, nullptr, 0, nullptr, nullptr);
            if (famLen > 0)
            {
                utf8Family.resize(famLen - 1);
                WideCharToMultiByte(CP_UTF8, 0, desc.FontFamily, -1, &utf8Family[0], famLen, nullptr, nullptr);
            }
        }

        int fontStyle[3] = { 400, 5, 0 }; // normal weight, normal width, upright
        sk_typeface_t* typeface = nullptr;
        if (!utf8Family.empty())
        {
            typeface = sk_typeface_create_from_name(utf8Family.c_str(), fontStyle);
        }
        if (!typeface)
        {
            typeface = sk_typeface_create_default();
        }

        sk_font_t* font = sk_font_new();
        sk_font_set_size(font, desc.FontSize > 0.0f ? desc.FontSize : 36.0f);
        if (typeface)
        {
            sk_font_set_typeface(font, typeface);
        }

        sk_paint_t* paint = sk_paint_new();
        sk_paint_set_antialias(paint, true);

        float baselineY = desc.Y + (desc.FontSize > 0.0f ? desc.FontSize : 36.0f);

        // Stroke Outline Pass
        if (desc.StrokeColor != 0 && desc.StrokeWidth > 0.0f)
        {
            sk_paint_set_color(paint, desc.StrokeColor);
            sk_paint_set_style(paint, SK_PAINT_STYLE_STROKE);
            sk_paint_set_stroke_width(paint, desc.StrokeWidth);
            sk_canvas_draw_simple_text(
                m_skCanvas,
                utf8Text.c_str(),
                utf8Text.length(),
                SK_TEXT_ENCODING_UTF8,
                desc.X,
                baselineY,
                font,
                paint);
        }

        // Fill Pass
        uint32_t fillCol = (desc.FillColor != 0) ? desc.FillColor : 0xFFFFFFFF;
        sk_paint_set_color(paint, fillCol);
        sk_paint_set_style(paint, SK_PAINT_STYLE_FILL);
        sk_canvas_draw_simple_text(
            m_skCanvas,
            utf8Text.c_str(),
            utf8Text.length(),
            SK_TEXT_ENCODING_UTF8,
            desc.X,
            baselineY,
            font,
            paint);

        sk_paint_delete(paint);
        sk_font_delete(font);
        if (typeface)
        {
            sk_typeface_unref(typeface);
        }
    }
    else if (desc.ElementType == 1) // Rectangle
    {
        sk_rect_t rect = { desc.X, desc.Y, desc.X + desc.Width, desc.Y + desc.Height };
        sk_rrect_t* rrect = sk_rrect_new();
        sk_rrect_set_rect_xy(rrect, &rect, 10.0f, 10.0f);

        sk_paint_t* paint = sk_paint_new();
        sk_paint_set_antialias(paint, true);

        // Fill
        if (desc.FillColor != 0)
        {
            sk_paint_set_style(paint, SK_PAINT_STYLE_FILL);
            sk_paint_set_color(paint, desc.FillColor);
            sk_canvas_draw_rrect(m_skCanvas, rrect, paint);
        }

        // Stroke
        if (desc.StrokeColor != 0 && desc.StrokeWidth > 0.0f)
        {
            sk_paint_set_style(paint, SK_PAINT_STYLE_STROKE);
            sk_paint_set_color(paint, desc.StrokeColor);
            sk_paint_set_stroke_width(paint, desc.StrokeWidth);
            sk_canvas_draw_rrect(m_skCanvas, rrect, paint);
        }

        sk_paint_delete(paint);
        sk_rrect_delete(rrect);
    }
    else if (desc.ElementType == 2) // Circle
    {
        float rx = desc.Width * 0.5f;
        float ry = desc.Height * 0.5f;
        float cx = desc.X + rx;
        float cy = desc.Y + ry;

        sk_paint_t* paint = sk_paint_new();
        sk_paint_set_antialias(paint, true);

        // Fill
        if (desc.FillColor != 0)
        {
            sk_paint_set_style(paint, SK_PAINT_STYLE_FILL);
            sk_paint_set_color(paint, desc.FillColor);
            sk_canvas_draw_circle(m_skCanvas, cx, cy, rx, paint);
        }

        // Stroke
        if (desc.StrokeColor != 0 && desc.StrokeWidth > 0.0f)
        {
            sk_paint_set_style(paint, SK_PAINT_STYLE_STROKE);
            sk_paint_set_color(paint, desc.StrokeColor);
            sk_paint_set_stroke_width(paint, desc.StrokeWidth);
            sk_canvas_draw_circle(m_skCanvas, cx, cy, rx, paint);
        }

        sk_paint_delete(paint);
    }

    // Render selection bounding box and handles if element is selected
    if (desc.IsSelected)
    {
        sk_paint_t* selPaint = sk_paint_new();
        sk_paint_set_color(selPaint, 0xFF4CC2FF); // WinUI accent blue
        sk_paint_set_style(selPaint, SK_PAINT_STYLE_STROKE);
        sk_paint_set_stroke_width(selPaint, 1.5f);
        sk_paint_set_antialias(selPaint, true);

        sk_rect_t boxRect = { desc.X, desc.Y, desc.X + desc.Width, desc.Y + desc.Height };
        sk_canvas_draw_rect(m_skCanvas, &boxRect, selPaint);

        sk_paint_t* handleFill = sk_paint_new();
        sk_paint_set_color(handleFill, 0xFFFFFFFF);
        sk_paint_set_style(handleFill, SK_PAINT_STYLE_FILL);
        sk_paint_set_antialias(handleFill, true);

        sk_paint_t* handleStroke = sk_paint_new();
        sk_paint_set_color(handleStroke, 0xFF0078D4);
        sk_paint_set_style(handleStroke, SK_PAINT_STYLE_STROKE);
        sk_paint_set_stroke_width(handleStroke, 1.0f);
        sk_paint_set_antialias(handleStroke, true);

        float hw = 4.0f; // 8x8 handles
        float xs[3] = { desc.X, desc.X + desc.Width * 0.5f, desc.X + desc.Width };
        float ys[3] = { desc.Y, desc.Y + desc.Height * 0.5f, desc.Y + desc.Height };

        for (int ix = 0; ix < 3; ++ix)
        {
            for (int iy = 0; iy < 3; ++iy)
            {
                if (ix == 1 && iy == 1) continue; // center is not a handle
                sk_rect_t hRect = { xs[ix] - hw, ys[iy] - hw, xs[ix] + hw, ys[iy] + hw };
                sk_canvas_draw_rect(m_skCanvas, &hRect, handleFill);
                sk_canvas_draw_rect(m_skCanvas, &hRect, handleStroke);
            }
        }

        sk_paint_delete(selPaint);
        sk_paint_delete(handleFill);
        sk_paint_delete(handleStroke);
    }
}

void SkiaRenderer::EndDraw()
{
    std::lock_guard<std::mutex> lock(m_lock);

    if (!m_skCanvas || !m_currentTargetTexture || !m_stagingTexture || !m_d3dDevice) return;

    // 1. Flush Skia drawing commands
    sk_canvas_flush(m_skCanvas);
    if (m_grContext)
    {
        gr_direct_context_flush_and_submit(m_grContext, true);
    }

    // 2. Commit Skia raster pixels directly into GPU staging texture
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    m_d3dDevice->GetImmediateContext(&context);

    D3D11_TEXTURE2D_DESC stagingDesc = {};
    m_stagingTexture->GetDesc(&stagingDesc);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context->Map(m_stagingTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(m_pixelBuffer.data());
        uint8_t* dst = reinterpret_cast<uint8_t*>(mapped.pData);

        int copyHeight = (std::min)(static_cast<int>(stagingDesc.Height), m_height);
        size_t rowBytes = (std::min)(static_cast<size_t>(stagingDesc.Width), static_cast<size_t>(m_width)) * sizeof(uint32_t);
        rowBytes = (std::min)(rowBytes, static_cast<size_t>(mapped.RowPitch));
        size_t totalSrcBytes = m_pixelBuffer.size() * sizeof(uint32_t);

        for (int y = 0; y < copyHeight; ++y)
        {
            size_t srcOffset = static_cast<size_t>(y) * m_width * sizeof(uint32_t);
            if (srcOffset + rowBytes <= totalSrcBytes)
            {
                memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch, src + srcOffset, rowBytes);
            }
        }

        context->Unmap(m_stagingTexture.Get(), 0);
    }

    // 3. Composite Skia overlay onto targetTexture on the GPU with Alpha Blending
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> targetRtv;
    hr = m_d3dDevice->CreateRenderTargetView(m_currentTargetTexture, nullptr, &targetRtv);
    if (SUCCEEDED(hr) && m_vsQuad && m_psBlend && m_blendState && m_sampler && m_stagingSrv)
    {
        D3D11_TEXTURE2D_DESC targetDesc;
        m_currentTargetTexture->GetDesc(&targetDesc);

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(targetDesc.Width);
        vp.Height = static_cast<float>(targetDesc.Height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);

        ID3D11RenderTargetView* rtvArray[1] = { targetRtv.Get() };
        context->OMSetRenderTargets(1, rtvArray, nullptr);

        float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context->OMSetBlendState(m_blendState.Get(), blendFactor, 0xFFFFFFFF);

        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        context->VSSetShader(m_vsQuad.Get(), nullptr, 0);
        context->PSSetShader(m_psBlend.Get(), nullptr, 0);

        ID3D11ShaderResourceView* srvs[1] = { m_stagingSrv.Get() };
        context->PSSetShaderResources(0, 1, srvs);

        ID3D11SamplerState* samplers[1] = { m_sampler.Get() };
        context->PSSetSamplers(0, 1, samplers);

        context->Draw(3, 0);

        // Reset state
        ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
        context->PSSetShaderResources(0, 1, nullSrv);

        ID3D11RenderTargetView* nullRtv[1] = { nullptr };
        context->OMSetRenderTargets(1, nullRtv, nullptr);
    }

    m_currentTargetTexture = nullptr;
}

void SkiaRenderer::EnumerateFonts(FontEnumerationCallback callback, void* userData)
{
    if (!callback) return;

    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory;
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &dwriteFactory);
    if (FAILED(hr)) return;

    Microsoft::WRL::ComPtr<IDWriteFontCollection> fontCollection;
    hr = dwriteFactory->GetSystemFontCollection(&fontCollection, FALSE);
    if (FAILED(hr)) return;

    UINT32 familyCount = fontCollection->GetFontFamilyCount();
    for (UINT32 i = 0; i < familyCount; ++i)
    {
        Microsoft::WRL::ComPtr<IDWriteFontFamily> fontFamily;
        hr = fontCollection->GetFontFamily(i, &fontFamily);
        if (FAILED(hr)) continue;

        Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> names;
        hr = fontFamily->GetFamilyNames(&names);
        if (FAILED(hr)) continue;

        UINT32 nameLen = 0;
        hr = names->GetStringLength(0, &nameLen);
        if (SUCCEEDED(hr) && nameLen > 0)
        {
            std::vector<wchar_t> nameBuf(nameLen + 1);
            if (SUCCEEDED(names->GetString(0, nameBuf.data(), nameLen + 1)))
            {
                callback(nameBuf.data(), userData);
            }
        }
    }
}
