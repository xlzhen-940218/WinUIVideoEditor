#include "FFmpegEncoder.h"
#include <iostream>
#include <vector>

FFmpegEncoder::FFmpegEncoder()
{
}

FFmpegEncoder::~FFmpegEncoder()
{
    CancelExport();
    if (m_workerThread.joinable())
    {
        m_workerThread.join();
    }
}

void FFmpegEncoder::CancelExport()
{
    m_cancelRequested.store(true);
}

const AVCodec* FFmpegEncoder::SelectEncoder(int hardwareEncoderType)
{
    const AVCodec* codec = nullptr;

    if (hardwareEncoderType == 1) // NVENC
    {
        codec = avcodec_find_encoder_by_name("h264_nvenc");
    }
    else if (hardwareEncoderType == 2) // AMF
    {
        codec = avcodec_find_encoder_by_name("h264_amf");
    }
    else if (hardwareEncoderType == 3) // QSV
    {
        codec = avcodec_find_encoder_by_name("h264_qsv");
    }

    if (!codec)
    {
        // Auto: try AMF (AMD GPU is primary on current platform)
        codec = avcodec_find_encoder_by_name("h264_amf");
        if (!codec) codec = avcodec_find_encoder_by_name("h264_nvenc");
        if (!codec) codec = avcodec_find_encoder_by_name("h264_qsv");
        if (!codec) codec = avcodec_find_encoder_by_name("h264_mf");
    }

    // Fallback to software encoders
    if (!codec) codec = avcodec_find_encoder_by_name("libopenh264");
    if (!codec) codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);

    return codec;
}

bool FFmpegEncoder::StartExport(
    ID3D11Device* device,
    VideoColorConverter* colorConverter,
    SkiaRenderer* skiaRenderer,
    FFmpegDecoder* decoder,
    const std::vector<PrimitiveElementDesc>& elements,
    const ExportConfig& config,
    ExportProgressCallback callback)
{
    if (m_isExporting.load())
    {
        return false;
    }

    if (m_workerThread.joinable())
    {
        m_workerThread.join();
    }

    m_isExporting.store(true);
    m_cancelRequested.store(false);

    m_workerThread = std::thread(
        &FFmpegEncoder::ExportWorker,
        this,
        device,
        colorConverter,
        skiaRenderer,
        decoder,
        elements,
        config,
        callback);

    return true;
}

void FFmpegEncoder::ExportWorker(
    ID3D11Device* device,
    VideoColorConverter* colorConverter,
    SkiaRenderer* skiaRenderer,
    FFmpegDecoder* decoder,
    std::vector<PrimitiveElementDesc> elements,
    ExportConfig config,
    ExportProgressCallback callback)
{
    int width = config.Width > 0 ? config.Width : (decoder && decoder->GetWidth() > 0 ? decoder->GetWidth() : 1920);
    int height = config.Height > 0 ? config.Height : (decoder && decoder->GetHeight() > 0 ? decoder->GetHeight() : 1080);
    // Align to even dimensions for H.264
    width = (width / 2) * 2;
    height = (height / 2) * 2;

    int fps = config.Fps > 0 ? config.Fps : 30;
    int bitrate = config.Bitrate > 0 ? config.Bitrate : 8000000;

    int64_t totalDurationMs = (decoder && decoder->GetDurationMs() > 0) ? decoder->GetDurationMs() : 10000;
    int64_t totalFrames = (totalDurationMs * fps) / 1000;
    if (totalFrames <= 0) totalFrames = 1;

    // Convert output path to UTF-8
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, config.OutputPath, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8Path(utf8Len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, config.OutputPath, -1, &utf8Path[0], utf8Len, nullptr, nullptr);

    const AVCodec* encoder = SelectEncoder(config.HardwareEncoderType);
    if (!encoder)
    {
        if (callback) callback(0.0f, true, -1);
        m_isExporting.store(false);
        return;
    }

    AVFormatContext* outFormatCtx = nullptr;
    if (avformat_alloc_output_context2(&outFormatCtx, nullptr, nullptr, utf8Path.c_str()) < 0 || !outFormatCtx)
    {
        if (callback) callback(0.0f, true, -2);
        m_isExporting.store(false);
        return;
    }

    AVStream* outStream = avformat_new_stream(outFormatCtx, nullptr);
    if (!outStream)
    {
        avformat_free_context(outFormatCtx);
        if (callback) callback(0.0f, true, -3);
        m_isExporting.store(false);
        return;
    }

    AVCodecContext* encCtx = avcodec_alloc_context3(encoder);
    if (!encCtx)
    {
        avformat_free_context(outFormatCtx);
        if (callback) callback(0.0f, true, -4);
        m_isExporting.store(false);
        return;
    }

    encCtx->codec_id = encoder->id;
    encCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    encCtx->width = width;
    encCtx->height = height;
    encCtx->time_base = { 1, fps };
    encCtx->framerate = { fps, 1 };
    encCtx->bit_rate = bitrate;
    encCtx->gop_size = fps * 2;
    encCtx->max_b_frames = 2;

    // Determine pixel format supported by encoder
    AVPixelFormat pixFmt = AV_PIX_FMT_NV12;
    if (encoder->pix_fmts)
    {
        bool nv12Supported = false;
        bool yuv420pSupported = false;
        for (const AVPixelFormat* p = encoder->pix_fmts; *p != AV_PIX_FMT_NONE; ++p)
        {
            if (*p == AV_PIX_FMT_NV12) nv12Supported = true;
            if (*p == AV_PIX_FMT_YUV420P) yuv420pSupported = true;
        }
        if (nv12Supported) pixFmt = AV_PIX_FMT_NV12;
        else if (yuv420pSupported) pixFmt = AV_PIX_FMT_YUV420P;
        else pixFmt = encoder->pix_fmts[0];
    }
    encCtx->pix_fmt = pixFmt;

    if (outFormatCtx->oformat->flags & AVFMT_GLOBALHEADER)
    {
        encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Set encoder preset for fast hardware encoding
    av_opt_set(encCtx->priv_data, "preset", "fast", 0);
    av_opt_set(encCtx->priv_data, "quality", "speed", 0);

    if (avcodec_open2(encCtx, encoder, nullptr) < 0)
    {
        avcodec_free_context(&encCtx);
        avformat_free_context(outFormatCtx);
        if (callback) callback(0.0f, true, -5);
        m_isExporting.store(false);
        return;
    }

    avcodec_parameters_from_context(outStream->codecpar, encCtx);
    outStream->time_base = encCtx->time_base;

    if (!(outFormatCtx->oformat->flags & AVFMT_NOFILE))
    {
        if (avio_open(&outFormatCtx->pb, utf8Path.c_str(), AVIO_FLAG_WRITE) < 0)
        {
            avcodec_free_context(&encCtx);
            avformat_free_context(outFormatCtx);
            if (callback) callback(0.0f, true, -6);
            m_isExporting.store(false);
            return;
        }
    }

    if (avformat_write_header(outFormatCtx, nullptr) < 0)
    {
        if (outFormatCtx->pb) avio_closep(&outFormatCtx->pb);
        avcodec_free_context(&encCtx);
        avformat_free_context(outFormatCtx);
        if (callback) callback(0.0f, true, -7);
        m_isExporting.store(false);
        return;
    }

    // Allocate offscreen textures on GPU
    Microsoft::WRL::ComPtr<ID3D11Texture2D> offscreenTarget;
    D3D11_TEXTURE2D_DESC descRGBA = {};
    descRGBA.Width = width;
    descRGBA.Height = height;
    descRGBA.MipLevels = 1;
    descRGBA.ArraySize = 1;
    descRGBA.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    descRGBA.SampleDesc.Count = 1;
    descRGBA.Usage = D3D11_USAGE_DEFAULT;
    descRGBA.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    device->CreateTexture2D(&descRGBA, nullptr, &offscreenTarget);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> offscreenNV12;
    D3D11_TEXTURE2D_DESC descNV12 = {};
    descNV12.Width = width;
    descNV12.Height = height;
    descNV12.MipLevels = 1;
    descNV12.ArraySize = 1;
    descNV12.Format = DXGI_FORMAT_NV12;
    descNV12.SampleDesc.Count = 1;
    descNV12.Usage = D3D11_USAGE_DEFAULT;
    descNV12.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    device->CreateTexture2D(&descNV12, nullptr, &offscreenNV12);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTexture;
    D3D11_TEXTURE2D_DESC descStaging = descNV12;
    descStaging.Usage = D3D11_USAGE_STAGING;
    descStaging.BindFlags = 0;
    descStaging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    device->CreateTexture2D(&descStaging, nullptr, &stagingTexture);

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediateCtx;
    device->GetImmediateContext(&immediateCtx);

    AVFrame* encFrame = av_frame_alloc();
    encFrame->format = encCtx->pix_fmt;
    encFrame->width = width;
    encFrame->height = height;
    av_frame_get_buffer(encFrame, 0);

    AVPacket* encPacket = av_packet_alloc();
    SwsContext* swsCtx = nullptr;

    for (int64_t i = 0; i < totalFrames; ++i)
    {
        if (m_cancelRequested.load())
        {
            break;
        }

        int64_t ptsMs = (i * 1000) / fps;

        // 1. Decode video frame at current timestamp
        if (decoder && decoder->IsLoaded())
        {
            decoder->Seek(ptsMs);
            decoder->ReadAndRenderFrame(offscreenTarget.Get(), width, height);
        }

        // 2. Skia 2D overlay rendering (Text & Shapes)
        if (skiaRenderer && offscreenTarget)
        {
            if (skiaRenderer->BeginDraw(offscreenTarget.Get()))
            {
                for (const auto& elem : elements)
                {
                    if (ptsMs >= elem.StartTimeMs && ptsMs <= (elem.StartTimeMs + elem.DurationMs))
                    {
                        skiaRenderer->DrawPrimitive(elem);
                    }
                }
                skiaRenderer->EndDraw();
            }
        }

        // 3. GPU VRAM Color Conversion RGBA8 -> NV12
        if (colorConverter && offscreenTarget && offscreenNV12)
        {
            colorConverter->ConvertRGBA8ToNV12(
                offscreenTarget.Get(), 
                offscreenNV12.Get(), 
                0, 
                width, 
                height, 
                width, 
                height);
        }

        // Copy NV12 texture to staging and fill encFrame
        immediateCtx->CopyResource(stagingTexture.Get(), offscreenNV12.Get());
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(immediateCtx->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        {
            const uint8_t* srcData[4] = {
                static_cast<const uint8_t*>(mapped.pData),
                static_cast<const uint8_t*>(mapped.pData) + (mapped.RowPitch * height),
                nullptr,
                nullptr
            };
            const int srcLinesize[4] = {
                static_cast<int>(mapped.RowPitch),
                static_cast<int>(mapped.RowPitch),
                0,
                0
            };

            if (encCtx->pix_fmt == AV_PIX_FMT_NV12)
            {
                av_image_copy(
                    encFrame->data,
                    encFrame->linesize,
                    srcData,
                    srcLinesize,
                    AV_PIX_FMT_NV12,
                    width,
                    height);
            }
            else
            {
                if (!swsCtx)
                {
                    swsCtx = sws_getContext(
                        width, height, AV_PIX_FMT_NV12,
                        width, height, encCtx->pix_fmt,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                }
                if (swsCtx)
                {
                    sws_scale(swsCtx, srcData, srcLinesize, 0, height, encFrame->data, encFrame->linesize);
                }
            }

            immediateCtx->Unmap(stagingTexture.Get(), 0);
        }

        encFrame->pts = i;

        // 4. Send frame to hardware encoder
        int ret = avcodec_send_frame(encCtx, encFrame);
        if (ret >= 0)
        {
            while (ret >= 0)
            {
                ret = avcodec_receive_packet(encCtx, encPacket);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                else if (ret < 0)
                    break;

                av_packet_rescale_ts(encPacket, encCtx->time_base, outStream->time_base);
                encPacket->stream_index = outStream->index;
                av_interleaved_write_frame(outFormatCtx, encPacket);
                av_packet_unref(encPacket);
            }
        }

        // Notify progress callback
        float progress = static_cast<float>(i + 1) / static_cast<float>(totalFrames);
        if (callback)
        {
            callback(progress, false, 0);
        }
    }

    // Flush encoder
    avcodec_send_frame(encCtx, nullptr);
    while (avcodec_receive_packet(encCtx, encPacket) >= 0)
    {
        av_packet_rescale_ts(encPacket, encCtx->time_base, outStream->time_base);
        encPacket->stream_index = outStream->index;
        av_interleaved_write_frame(outFormatCtx, encPacket);
        av_packet_unref(encPacket);
    }

    av_write_trailer(outFormatCtx);

    av_packet_free(&encPacket);
    av_frame_free(&encFrame);
    if (swsCtx)
    {
        sws_freeContext(swsCtx);
        swsCtx = nullptr;
    }

    if (!(outFormatCtx->oformat->flags & AVFMT_NOFILE))
    {
        avio_closep(&outFormatCtx->pb);
    }
    avcodec_free_context(&encCtx);
    avformat_free_context(outFormatCtx);

    bool cancelled = m_cancelRequested.load();
    if (callback)
    {
        callback(cancelled ? 0.0f : 1.0f, true, cancelled ? -99 : 0);
    }

    m_isExporting.store(false);
}
