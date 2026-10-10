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
    const std::vector<VideoClipDesc>& videoClips,
    const std::vector<VideoClipDesc>& audioClips,
    const std::vector<PrimitiveElementDesc>& elements,
    const ExportConfig& config,
    ExportProgressCallback callback,
    int previewWidth,
    int previewHeight)
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
        videoClips,
        audioClips,
        elements,
        config,
        callback,
        previewWidth,
        previewHeight);

    return true;
}

void FFmpegEncoder::ExportWorker(
    ID3D11Device* device,
    VideoColorConverter* colorConverter,
    SkiaRenderer* skiaRenderer,
    FFmpegDecoder* decoder,
    std::vector<VideoClipDesc> videoClips,
    std::vector<VideoClipDesc> audioClips,
    std::vector<PrimitiveElementDesc> elements,
    ExportConfig config,
    ExportProgressCallback callback,
    int previewWidth,
    int previewHeight)
{
    int width = config.Width > 0 ? config.Width : (decoder && decoder->GetWidth() > 0 ? decoder->GetWidth() : 1920);
    int height = config.Height > 0 ? config.Height : (decoder && decoder->GetHeight() > 0 ? decoder->GetHeight() : 1080);
    // Align to even dimensions for H.264
    width = (width / 2) * 2;
    height = (height / 2) * 2;

    int fps = config.Fps > 0 ? config.Fps : 30;
    int bitrate = config.Bitrate > 0 ? config.Bitrate : 8000000;

    int64_t totalDurationMs = 0;
    if (!videoClips.empty())
    {
        for (const auto& c : videoClips)
        {
            totalDurationMs = (std::max)(totalDurationMs, c.StartTimeMs + c.DurationMs);
        }
    }
    if (totalDurationMs <= 0 && decoder && decoder->GetDurationMs() > 0)
    {
        totalDurationMs = decoder->GetDurationMs();
    }
    if (totalDurationMs <= 0) totalDurationMs = 10000;

    int64_t totalFrames = (totalDurationMs * fps) / 1000;
    if (totalFrames <= 0) totalFrames = 1;

    // Calculate aspect-ratio fit to accurately scale Skia elements without any Y-axis distortion
    int vidW = width;
    int vidH = height;
    if (decoder && decoder->GetWidth() > 0 && decoder->GetHeight() > 0)
    {
        vidW = decoder->GetWidth();
        vidH = decoder->GetHeight();
    }

    int prevW = previewWidth > 0 ? previewWidth : width;
    int prevH = previewHeight > 0 ? previewHeight : height;

    double srcAspect = static_cast<double>(vidW) / static_cast<double>(vidH);
    double prevAspect = static_cast<double>(prevW) / static_cast<double>(prevH);

    int prevFitW = prevW;
    int prevFitH = prevH;
    int prevOffsetX = 0;
    int prevOffsetY = 0;

    if (srcAspect > prevAspect)
    {
        prevFitW = prevW;
        prevFitH = (std::max)(1, static_cast<int>(std::round(prevW / srcAspect)));
        prevOffsetY = (prevH - prevFitH) / 2;
    }
    else
    {
        prevFitH = prevH;
        prevFitW = (std::max)(1, static_cast<int>(std::round(prevH * srcAspect)));
        prevOffsetX = (prevW - prevFitW) / 2;
    }

    double expAspect = static_cast<double>(width) / static_cast<double>(height);
    int expFitW = width;
    int expFitH = height;
    int expOffsetX = 0;
    int expOffsetY = 0;

    if (srcAspect > expAspect)
    {
        expFitW = width;
        expFitH = (std::max)(1, static_cast<int>(std::round(width / srcAspect)));
        expOffsetY = (height - expFitH) / 2;
    }
    else
    {
        expFitH = height;
        expFitW = (std::max)(1, static_cast<int>(std::round(height * srcAspect)));
        expOffsetX = (width - expFitW) / 2;
    }

    std::vector<PrimitiveElementDesc> scaledElements;
    for (const auto& el : elements)
    {
        PrimitiveElementDesc s = el;
        s.IsSelected = 0; // Never render selection handles during export
        float normX = (el.X - prevOffsetX) / static_cast<float>(prevFitW);
        float normY = (el.Y - prevOffsetY) / static_cast<float>(prevFitH);
        float normW = el.Width / static_cast<float>(prevFitW);
        float normH = el.Height / static_cast<float>(prevFitH);

        s.X = expOffsetX + normX * expFitW;
        s.Y = expOffsetY + normY * expFitH;
        s.Width = normW * expFitW;
        s.Height = normH * expFitH;
        float scale = static_cast<float>(expFitH) / static_cast<float>(prevFitH);
        s.FontSize = el.FontSize * scale;
        s.StrokeWidth = el.StrokeWidth * scale;
        scaledElements.push_back(s);
    }

    // Resize Skia to full export resolution for clean vector rasterization
    if (skiaRenderer)
    {
        skiaRenderer->Resize(width, height);
    }

    // Convert output path to UTF-8
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, config.OutputPath, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8Path(utf8Len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, config.OutputPath, -1, &utf8Path[0], utf8Len, nullptr, nullptr);

    const AVCodec* encoder = SelectEncoder(config.HardwareEncoderType);
    if (!encoder)
    {
        if (skiaRenderer) skiaRenderer->Resize(prevW, prevH);
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

    // Audio Extraction & Transcoding Preparation
    std::vector<VideoClipDesc> effAudioClips = audioClips.empty() ? videoClips : audioClips;
    int64_t totalAudioSamples = (totalDurationMs * 44100) / 1000;
    std::vector<float> pcmL;
    std::vector<float> pcmR;
    bool hasAudioContent = false;

    if (totalAudioSamples > 0 && !effAudioClips.empty())
    {
        pcmL.assign(static_cast<size_t>(totalAudioSamples), 0.0f);
        pcmR.assign(static_cast<size_t>(totalAudioSamples), 0.0f);

        for (const auto& aClip : effAudioClips)
        {
            int uLen = WideCharToMultiByte(CP_UTF8, 0, aClip.FilePath, -1, nullptr, 0, nullptr, nullptr);
            if (uLen <= 0) continue;
            std::string uPath(uLen, '\0');
            WideCharToMultiByte(CP_UTF8, 0, aClip.FilePath, -1, &uPath[0], uLen, nullptr, nullptr);

            AVFormatContext* aInFmt = nullptr;
            if (avformat_open_input(&aInFmt, uPath.c_str(), nullptr, nullptr) != 0 || !aInFmt)
            {
                continue;
            }
            if (avformat_find_stream_info(aInFmt, nullptr) < 0)
            {
                avformat_close_input(&aInFmt);
                continue;
            }

            const AVCodec* aDec = nullptr;
            int aStreamIdx = av_find_best_stream(aInFmt, AVMEDIA_TYPE_AUDIO, -1, -1, &aDec, 0);
            if (aStreamIdx < 0 || !aDec)
            {
                avformat_close_input(&aInFmt);
                continue;
            }

            AVCodecContext* aDecCtx = avcodec_alloc_context3(aDec);
            if (!aDecCtx)
            {
                avformat_close_input(&aInFmt);
                continue;
            }
            if (avcodec_parameters_to_context(aDecCtx, aInFmt->streams[aStreamIdx]->codecpar) < 0 ||
                avcodec_open2(aDecCtx, aDec, nullptr) < 0)
            {
                avcodec_free_context(&aDecCtx);
                avformat_close_input(&aInFmt);
                continue;
            }

            SwrContext* swr = nullptr;
            AVChannelLayout stereoLayout;
            av_channel_layout_default(&stereoLayout, 2);
            swr_alloc_set_opts2(&swr, &stereoLayout, AV_SAMPLE_FMT_FLTP, 44100,
                                &aDecCtx->ch_layout, aDecCtx->sample_fmt, aDecCtx->sample_rate,
                                0, nullptr);
            if (!swr || swr_init(swr) < 0)
            {
                if (swr) swr_free(&swr);
                avcodec_free_context(&aDecCtx);
                avformat_close_input(&aInFmt);
                continue;
            }

            hasAudioContent = true;

            AVFrame* aFrame = av_frame_alloc();
            AVPacket* aPkt = av_packet_alloc();

            int64_t clipStartSample = (aClip.StartTimeMs * 44100) / 1000;
            int64_t currentSampleOffset = clipStartSample;
            int64_t clipEndSample = ((aClip.StartTimeMs + aClip.DurationMs) * 44100) / 1000;

            std::vector<float> resampleL(4096);
            std::vector<float> resampleR(4096);
            uint8_t* outPlanes[2] = { reinterpret_cast<uint8_t*>(resampleL.data()), reinterpret_cast<uint8_t*>(resampleR.data()) };

            while (av_read_frame(aInFmt, aPkt) >= 0)
            {
                if (aPkt->stream_index == aStreamIdx)
                {
                    if (avcodec_send_packet(aDecCtx, aPkt) >= 0)
                    {
                        while (avcodec_receive_frame(aDecCtx, aFrame) >= 0)
                        {
                            int outCount = swr_convert(swr, outPlanes, 4096,
                                const_cast<const uint8_t**>(aFrame->data), aFrame->nb_samples);
                            for (int s = 0; s < outCount; ++s)
                            {
                                int64_t targetIdx = currentSampleOffset + s;
                                if (targetIdx >= 0 && targetIdx < totalAudioSamples && targetIdx < clipEndSample)
                                {
                                    pcmL[targetIdx] = resampleL[s];
                                    pcmR[targetIdx] = resampleR[s];
                                }
                            }
                            currentSampleOffset += outCount;
                            av_frame_unref(aFrame);
                        }
                    }
                }
                av_packet_unref(aPkt);
            }

            // Flush decoder
            avcodec_send_packet(aDecCtx, nullptr);
            while (avcodec_receive_frame(aDecCtx, aFrame) >= 0)
            {
                int outCount = swr_convert(swr, outPlanes, 4096,
                    const_cast<const uint8_t**>(aFrame->data), aFrame->nb_samples);
                for (int s = 0; s < outCount; ++s)
                {
                    int64_t targetIdx = currentSampleOffset + s;
                    if (targetIdx >= 0 && targetIdx < totalAudioSamples && targetIdx < clipEndSample)
                    {
                        pcmL[targetIdx] = resampleL[s];
                        pcmR[targetIdx] = resampleR[s];
                    }
                }
                currentSampleOffset += outCount;
                av_frame_unref(aFrame);
            }

            av_frame_free(&aFrame);
            av_packet_free(&aPkt);
            swr_free(&swr);
            avcodec_free_context(&aDecCtx);
            avformat_close_input(&aInFmt);
        }
    }

    AVStream* outAudioStream = nullptr;
    AVCodecContext* aacEncCtx = nullptr;
    const AVCodec* aacEncoder = avcodec_find_encoder(AV_CODEC_ID_AAC);

    if (hasAudioContent && aacEncoder)
    {
        outAudioStream = avformat_new_stream(outFormatCtx, nullptr);
        if (outAudioStream)
        {
            aacEncCtx = avcodec_alloc_context3(aacEncoder);
            if (aacEncCtx)
            {
                aacEncCtx->codec_id = AV_CODEC_ID_AAC;
                aacEncCtx->codec_type = AVMEDIA_TYPE_AUDIO;
                aacEncCtx->sample_rate = 44100;
                aacEncCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
                aacEncCtx->bit_rate = 192000;
                av_channel_layout_default(&aacEncCtx->ch_layout, 2);
                aacEncCtx->time_base = { 1, 44100 };
                outAudioStream->time_base = aacEncCtx->time_base;

                if (outFormatCtx->oformat->flags & AVFMT_GLOBALHEADER)
                {
                    aacEncCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                }

                if (avcodec_open2(aacEncCtx, aacEncoder, nullptr) >= 0)
                {
                    avcodec_parameters_from_context(outAudioStream->codecpar, aacEncCtx);
                }
                else
                {
                    avcodec_free_context(&aacEncCtx);
                    aacEncCtx = nullptr;
                }
            }
        }
    }

    if (avformat_write_header(outFormatCtx, nullptr) < 0)
    {
        if (outFormatCtx->pb) avio_closep(&outFormatCtx->pb);
        if (aacEncCtx) avcodec_free_context(&aacEncCtx);
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

    int currentClipIdx = -1;
    if (!videoClips.empty())
    {
        decoder->OpenFile(videoClips[0].FilePath);
        currentClipIdx = 0;
        decoder->Seek(0);
    }
    else if (decoder && decoder->IsLoaded())
    {
        decoder->Seek(0);
    }

    AVFrame* aacFrame = nullptr;
    AVPacket* aacPacket = nullptr;
    int64_t audioFrameIdx = 0;
    int64_t totalAudioFrames = (totalAudioSamples + 1023) / 1024;

    if (aacEncCtx && outAudioStream)
    {
        aacFrame = av_frame_alloc();
        aacFrame->nb_samples = 1024;
        aacFrame->format = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&aacFrame->ch_layout, 2);
        aacFrame->sample_rate = 44100;
        av_frame_get_buffer(aacFrame, 0);

        aacPacket = av_packet_alloc();
    }

    auto writeAudioFrame = [&](int64_t fIdx) {
        if (!aacEncCtx || !outAudioStream || !aacFrame || !aacPacket) return;
        int64_t sampleStart = fIdx * 1024;
        av_frame_make_writable(aacFrame);
        float* dstL = reinterpret_cast<float*>(aacFrame->data[0]);
        float* dstR = reinterpret_cast<float*>(aacFrame->data[1]);
        for (int s = 0; s < 1024; ++s)
        {
            int64_t srcIdx = sampleStart + s;
            if (srcIdx < totalAudioSamples)
            {
                dstL[s] = pcmL[srcIdx];
                dstR[s] = pcmR[srcIdx];
            }
            else
            {
                dstL[s] = 0.0f;
                dstR[s] = 0.0f;
            }
        }
        aacFrame->pts = sampleStart;
        if (avcodec_send_frame(aacEncCtx, aacFrame) >= 0)
        {
            while (avcodec_receive_packet(aacEncCtx, aacPacket) >= 0)
            {
                av_packet_rescale_ts(aacPacket, aacEncCtx->time_base, outAudioStream->time_base);
                aacPacket->stream_index = outAudioStream->index;
                av_interleaved_write_frame(outFormatCtx, aacPacket);
                av_packet_unref(aacPacket);
            }
        }
    };

    for (int64_t i = 0; i < totalFrames; ++i)
    {
        if (m_cancelRequested.load())
        {
            break;
        }

        int64_t ptsMs = (i * 1000) / fps;

        // Interleave audio frames up to current video timestamp
        while (audioFrameIdx < totalAudioFrames)
        {
            int64_t audioPtsMs = (audioFrameIdx * 1024 * 1000) / 44100;
            if (audioPtsMs <= ptsMs + 50)
            {
                writeAudioFrame(audioFrameIdx++);
            }
            else
            {
                break;
            }
        }

        // 1. Decode video frame at current timestamp
        if (!videoClips.empty())
        {
            int targetClip = -1;
            for (size_t c = 0; c < videoClips.size(); ++c)
            {
                if (ptsMs >= videoClips[c].StartTimeMs &&
                    ptsMs < (videoClips[c].StartTimeMs + videoClips[c].DurationMs))
                {
                    targetClip = static_cast<int>(c);
                    break;
                }
            }

            if (targetClip >= 0)
            {
                const auto& clip = videoClips[targetClip];
                int64_t localPts = ptsMs - clip.StartTimeMs;

                if (currentClipIdx != targetClip)
                {
                    decoder->OpenFile(clip.FilePath);
                    currentClipIdx = targetClip;
                    decoder->Seek(localPts);
                }
                else
                {
                    decoder->DecodeNextFrame(nullptr);
                }

                decoder->RenderCurrentFrame(offscreenTarget.Get(), width, height);
            }
            else
            {
                // Clear offscreenTarget with dark frame when outside clip ranges
                Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
                if (SUCCEEDED(device->CreateRenderTargetView(offscreenTarget.Get(), nullptr, &rtv)))
                {
                    const float black[4] = { 0.07f, 0.07f, 0.08f, 1.0f };
                    immediateCtx->ClearRenderTargetView(rtv.Get(), black);
                }
            }
        }
        else if (decoder && decoder->IsLoaded())
        {
            if (i == 0)
            {
                decoder->Seek(0);
            }
            else
            {
                decoder->DecodeNextFrame(nullptr);
            }
            decoder->RenderCurrentFrame(offscreenTarget.Get(), width, height);
        }

        // 2. Skia 2D overlay rendering (Text & Shapes) with pre-scaled coordinates
        if (skiaRenderer && offscreenTarget)
        {
            if (skiaRenderer->BeginDraw(offscreenTarget.Get()))
            {
                for (const auto& elem : scaledElements)
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

    // Finish any remaining audio frames
    while (audioFrameIdx < totalAudioFrames)
    {
        writeAudioFrame(audioFrameIdx++);
    }

    if (aacEncCtx && outAudioStream && aacPacket)
    {
        avcodec_send_frame(aacEncCtx, nullptr);
        while (avcodec_receive_packet(aacEncCtx, aacPacket) >= 0)
        {
            av_packet_rescale_ts(aacPacket, aacEncCtx->time_base, outAudioStream->time_base);
            aacPacket->stream_index = outAudioStream->index;
            av_interleaved_write_frame(outFormatCtx, aacPacket);
            av_packet_unref(aacPacket);
        }
    }

    av_write_trailer(outFormatCtx);

    if (aacFrame) av_frame_free(&aacFrame);
    if (aacPacket) av_packet_free(&aacPacket);
    if (aacEncCtx) avcodec_free_context(&aacEncCtx);

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

    if (skiaRenderer)
    {
        skiaRenderer->Resize(prevW, prevH);
    }

    m_isExporting.store(false);
}
