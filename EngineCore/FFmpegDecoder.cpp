#include "FFmpegDecoder.h"
#include <iostream>
#include <string>

AVPixelFormat FFmpegDecoder::GetHwFormat(AVCodecContext* ctx, const AVPixelFormat* pix_fmts)
{
    const AVPixelFormat* p;
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++)
    {
        if (*p == AV_PIX_FMT_D3D11)
            return *p;
    }
    return AV_PIX_FMT_NONE;
}

FFmpegDecoder::FFmpegDecoder()
{
}

FFmpegDecoder::~FFmpegDecoder()
{
    Cleanup();
}

bool FFmpegDecoder::Initialize(ID3D11Device* device, VideoColorConverter* converter)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    m_d3dDevice = device;
    m_colorConverter = converter;

    m_frame = av_frame_alloc();
    m_swFrame = av_frame_alloc();
    m_packet = av_packet_alloc();

    return m_frame && m_swFrame && m_packet;
}

void FFmpegDecoder::Cleanup()
{
    CloseFile();

    std::lock_guard<std::recursive_mutex> lock(m_lock);
    if (m_frame) { av_frame_free(&m_frame); m_frame = nullptr; }
    if (m_swFrame) { av_frame_free(&m_swFrame); m_swFrame = nullptr; }
    if (m_packet) { av_packet_free(&m_packet); m_packet = nullptr; }
}

void FFmpegDecoder::CloseFile()
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    if (m_swsCtx)
    {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    m_cpuUploadTexture.Reset();
    m_cachedFrameRgba.Reset();
    m_hasCachedFrame = false;

    if (m_codecCtx)
    {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    if (m_hwDeviceCtx)
    {
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = nullptr;
    }
    if (m_formatCtx)
    {
        avformat_close_input(&m_formatCtx);
        m_formatCtx = nullptr;
    }

    m_videoStreamIdx = -1;
    m_width = 0;
    m_height = 0;
    m_durationMs = 0;
    m_currentPosMs = 0;
}

bool FFmpegDecoder::OpenFile(const wchar_t* filePath)
{
    CloseFile();

    std::lock_guard<std::recursive_mutex> lock(m_lock);

    if (!filePath || !m_d3dDevice) return false;

    // Convert wchar_t path to UTF-8
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return false;
    std::string utf8Path(utf8Len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, filePath, -1, &utf8Path[0], utf8Len, nullptr, nullptr);

    if (avformat_open_input(&m_formatCtx, utf8Path.c_str(), nullptr, nullptr) != 0)
    {
        return false;
    }

    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0)
    {
        CloseFile();
        return false;
    }

    const AVCodec* decoder = nullptr;
    m_videoStreamIdx = av_find_best_stream(m_formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (m_videoStreamIdx < 0 || !decoder)
    {
        CloseFile();
        return false;
    }

    AVStream* videoStream = m_formatCtx->streams[m_videoStreamIdx];
    m_codecCtx = avcodec_alloc_context3(decoder);
    if (!m_codecCtx)
    {
        CloseFile();
        return false;
    }

    if (avcodec_parameters_to_context(m_codecCtx, videoStream->codecpar) < 0)
    {
        CloseFile();
        return false;
    }

    // Initialize D3D11VA Hardware Device Context
    m_hwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (m_hwDeviceCtx)
    {
        AVHWDeviceContext* deviceCtx = (AVHWDeviceContext*)m_hwDeviceCtx->data;
        AVD3D11VADeviceContext* d3d11vaDeviceCtx = (AVD3D11VADeviceContext*)deviceCtx->hwctx;
        d3d11vaDeviceCtx->device = m_d3dDevice;
        m_d3dDevice->AddRef();

        if (av_hwdevice_ctx_init(m_hwDeviceCtx) == 0)
        {
            m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
            m_codecCtx->get_format = GetHwFormat;
        }
        else
        {
            av_buffer_unref(&m_hwDeviceCtx);
            m_hwDeviceCtx = nullptr;
        }
    }

    if (avcodec_open2(m_codecCtx, decoder, nullptr) < 0)
    {
        CloseFile();
        return false;
    }

    m_width = m_codecCtx->width;
    m_height = m_codecCtx->height;

    if (videoStream->duration != AV_NOPTS_VALUE && videoStream->time_base.den > 0)
    {
        m_durationMs = static_cast<int64_t>(videoStream->duration * av_q2d(videoStream->time_base) * 1000.0);
    }
    else if (m_formatCtx->duration != AV_NOPTS_VALUE)
    {
        m_durationMs = m_formatCtx->duration / (AV_TIME_BASE / 1000);
    }

    if (videoStream->avg_frame_rate.den > 0 && videoStream->avg_frame_rate.num > 0)
    {
        m_fps = av_q2d(videoStream->avg_frame_rate);
    }
    else
    {
        m_fps = 30.0;
    }

    // Allocate GPU texture for caching decoded RGBA8 frames at native resolution
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = m_width > 0 ? m_width : 1280;
    desc.Height = m_height > 0 ? m_height : 720;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = m_d3dDevice->CreateTexture2D(&desc, nullptr, &m_cachedFrameRgba);
    if (FAILED(hr))
    {
        m_cachedFrameRgba.Reset();
    }

    // Immediately decode the very first frame (Frame 0) so canvas is never black
    m_hasCachedFrame = false;
    DecodeNextFrame(&m_currentPosMs);

    return true;
}

void FFmpegDecoder::UpdateCachedFrameFromAvFrame()
{
    if (!m_cachedFrameRgba || !m_colorConverter) return;

    if (m_frame->format == AV_PIX_FMT_D3D11)
    {
        ID3D11Texture2D* d3d11Tex = (ID3D11Texture2D*)m_frame->data[0];
        intptr_t subresource = (intptr_t)m_frame->data[1];

        if (d3d11Tex)
        {
            m_colorConverter->ConvertNV12ToRGBA8(
                d3d11Tex, 
                static_cast<UINT>(subresource), 
                m_cachedFrameRgba.Get(), 
                m_width, 
                m_height, 
                m_width, 
                m_height);
            m_hasCachedFrame = true;
        }
    }
    else
    {
        // Fallback for software decoded frames: upload to GPU texture
        if (!m_swsCtx)
        {
            m_swsCtx = sws_getContext(
                m_width, m_height, m_codecCtx->pix_fmt,
                m_width, m_height, AV_PIX_FMT_BGRA,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
        }

        if (!m_cpuUploadTexture)
        {
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
            m_d3dDevice->CreateTexture2D(&desc, nullptr, &m_cpuUploadTexture);
        }

        if (m_swsCtx && m_cpuUploadTexture)
        {
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
            m_d3dDevice->GetImmediateContext(&ctx);

            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(ctx->Map(m_cpuUploadTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                uint8_t* dstData[4] = { (uint8_t*)mapped.pData, nullptr, nullptr, nullptr };
                int dstLinesize[4] = { (int)mapped.RowPitch, 0, 0, 0 };
                sws_scale(m_swsCtx, m_frame->data, m_frame->linesize, 0, m_height, dstData, dstLinesize);
                ctx->Unmap(m_cpuUploadTexture.Get(), 0);

                ctx->CopyResource(m_cachedFrameRgba.Get(), m_cpuUploadTexture.Get());
                m_hasCachedFrame = true;
            }
        }
    }
}

bool FFmpegDecoder::DecodeNextFrame(int64_t* outPtsMs)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    if (!m_formatCtx || !m_codecCtx || m_videoStreamIdx < 0) return false;

    AVStream* videoStream = m_formatCtx->streams[m_videoStreamIdx];

    while (true)
    {
        int ret = avcodec_receive_frame(m_codecCtx, m_frame);
        if (ret == 0)
        {
            if (m_frame->pts != AV_NOPTS_VALUE)
            {
                m_currentPosMs = static_cast<int64_t>(m_frame->pts * av_q2d(videoStream->time_base) * 1000.0);
            }
            if (outPtsMs)
            {
                *outPtsMs = m_currentPosMs;
            }

            UpdateCachedFrameFromAvFrame();
            av_frame_unref(m_frame);
            return true;
        }

        if (ret == AVERROR_EOF)
        {
            return false;
        }

        // Read packet from stream
        int readRet = av_read_frame(m_formatCtx, m_packet);
        if (readRet < 0)
        {
            avcodec_send_packet(m_codecCtx, nullptr);
            if (readRet == AVERROR_EOF)
            {
                int flushRet = avcodec_receive_frame(m_codecCtx, m_frame);
                if (flushRet == 0)
                {
                    if (m_frame->pts != AV_NOPTS_VALUE)
                    {
                        m_currentPosMs = static_cast<int64_t>(m_frame->pts * av_q2d(videoStream->time_base) * 1000.0);
                    }
                    if (outPtsMs) *outPtsMs = m_currentPosMs;
                    UpdateCachedFrameFromAvFrame();
                    av_frame_unref(m_frame);
                    return true;
                }
                break;
            }
            return false;
        }

        if (m_packet->stream_index == m_videoStreamIdx)
        {
            avcodec_send_packet(m_codecCtx, m_packet);
        }
        av_packet_unref(m_packet);
    }

    return false;
}

bool FFmpegDecoder::RenderCurrentFrame(ID3D11Texture2D* targetTexture, int targetWidth, int targetHeight)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    if (!m_hasCachedFrame || !m_cachedFrameRgba || !m_colorConverter || !targetTexture)
    {
        return false;
    }

    return m_colorConverter->BlitRGBA8(
        m_cachedFrameRgba.Get(),
        targetTexture,
        m_width,
        m_height,
        targetWidth,
        targetHeight);
}

bool FFmpegDecoder::ReadAndRenderFrame(ID3D11Texture2D* targetTexture, int targetWidth, int targetHeight, int64_t* outPtsMs)
{
    DecodeNextFrame(outPtsMs);
    return RenderCurrentFrame(targetTexture, targetWidth, targetHeight);
}

bool FFmpegDecoder::Seek(int64_t timeStampMs)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    if (!m_formatCtx || !m_codecCtx || m_videoStreamIdx < 0) return false;

    AVStream* videoStream = m_formatCtx->streams[m_videoStreamIdx];
    int64_t targetPts = static_cast<int64_t>((timeStampMs / 1000.0) / av_q2d(videoStream->time_base));

    int ret = av_seek_frame(m_formatCtx, m_videoStreamIdx, targetPts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0)
    {
        ret = av_seek_frame(m_formatCtx, -1, targetPts, AVSEEK_FLAG_BACKWARD);
    }
    if (ret >= 0)
    {
        avcodec_flush_buffers(m_codecCtx);
        m_currentPosMs = timeStampMs;

        // Decode frame after seek to immediately update cached frame
        while (true)
        {
            int r = avcodec_receive_frame(m_codecCtx, m_frame);
            if (r == 0)
            {
                if (m_frame->pts != AV_NOPTS_VALUE)
                {
                    m_currentPosMs = static_cast<int64_t>(m_frame->pts * av_q2d(videoStream->time_base) * 1000.0);
                }
                UpdateCachedFrameFromAvFrame();
                av_frame_unref(m_frame);
                break;
            }

            int readRet = av_read_frame(m_formatCtx, m_packet);
            if (readRet < 0)
            {
                avcodec_send_packet(m_codecCtx, nullptr);
                int flushRet = avcodec_receive_frame(m_codecCtx, m_frame);
                if (flushRet == 0)
                {
                    if (m_frame->pts != AV_NOPTS_VALUE)
                    {
                        m_currentPosMs = static_cast<int64_t>(m_frame->pts * av_q2d(videoStream->time_base) * 1000.0);
                    }
                    UpdateCachedFrameFromAvFrame();
                    av_frame_unref(m_frame);
                }
                break;
            }
            if (m_packet->stream_index == m_videoStreamIdx)
            {
                avcodec_send_packet(m_codecCtx, m_packet);
            }
            av_packet_unref(m_packet);
        }

        return true;
    }

    return false;
}
