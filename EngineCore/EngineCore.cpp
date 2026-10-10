#include "EngineCore.h"
#include "D3D11Renderer.h"
#include "VideoProcessor.h"
#include "SkiaRenderer.h"
#include "FFmpegDecoder.h"
#include "FFmpegEncoder.h"
#include "AudioPlayer.h"

#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>

struct EngineContext
{
    HWND hWnd = nullptr;
    int width = 0;
    int height = 0;

    std::unique_ptr<D3D11Renderer> renderer;
    std::unique_ptr<VideoColorConverter> colorConverter;
    std::unique_ptr<SkiaRenderer> skiaRenderer;
    std::unique_ptr<FFmpegDecoder> decoder;
    std::unique_ptr<FFmpegEncoder> encoder;
    std::unique_ptr<AudioPlayer> audioPlayer;
    std::atomic<bool> audioDirty{ true };

    std::map<int, PrimitiveElementDesc> elements;
    std::mutex elementsLock;
    std::recursive_mutex renderLock;

    std::vector<VideoClipDesc> videoClips;
    std::mutex videoClipsLock;
    int currentClipIndex = -1;

    std::vector<VideoClipDesc> audioClips;
    std::mutex audioClipsLock;

    std::atomic<bool> isPlaying{ false };
    std::atomic<bool> stopPlaybackLoop{ false };
    std::thread playbackThread;

    int64_t currentPlaybackPosMs = 0;

    void RenderSingleFrame()
    {
        std::lock_guard<std::recursive_mutex> lock(renderLock);
        if (!renderer) return;

        renderer->BeginFrame();

        // 1. Advance timeline playback if playing
        if (isPlaying.load())
        {
            currentPlaybackPosMs += 33;
            int64_t totalDur = 0;
            {
                std::lock_guard<std::mutex> vLock(videoClipsLock);
                for (const auto& c : videoClips)
                {
                    totalDur = (std::max)(totalDur, c.StartTimeMs + c.DurationMs);
                }
            }
            if (totalDur <= 0 && decoder && decoder->GetDurationMs() > 0)
            {
                totalDur = decoder->GetDurationMs();
            }
            if (totalDur > 0 && currentPlaybackPosMs >= totalDur)
            {
                currentPlaybackPosMs = totalDur;
                isPlaying.store(false);
                if (audioPlayer) audioPlayer->Pause();
            }
        }

        // 2. Hardware video frame rendering (NV12 -> RGBA8 in VRAM)
        {
            std::lock_guard<std::mutex> vLock(videoClipsLock);
            if (!videoClips.empty())
            {
                int targetIdx = -1;
                for (size_t i = 0; i < videoClips.size(); ++i)
                {
                    if (currentPlaybackPosMs >= videoClips[i].StartTimeMs &&
                        currentPlaybackPosMs < (videoClips[i].StartTimeMs + videoClips[i].DurationMs))
                    {
                        targetIdx = static_cast<int>(i);
                        break;
                    }
                }

                if (targetIdx >= 0)
                {
                    const auto& clip = videoClips[targetIdx];
                    int64_t localOffset = currentPlaybackPosMs - clip.StartTimeMs;

                    if (currentClipIndex != targetIdx)
                    {
                        decoder->OpenFile(clip.FilePath);
                        currentClipIndex = targetIdx;
                        if (localOffset > 50)
                        {
                            decoder->Seek(localOffset);
                        }
                    }
                    else if (isPlaying.load())
                    {
                        int64_t clipFramePos = 0;
                        bool gotFrame = decoder->DecodeNextFrame(&clipFramePos);
                        if (gotFrame && clipFramePos > localOffset)
                        {
                            currentPlaybackPosMs = clip.StartTimeMs + clipFramePos;
                        }
                        else if (gotFrame)
                        {
                            currentPlaybackPosMs += 33;
                        }
                        else
                        {
                            // Reached EOF of current clip, advance to the next clip
                            currentPlaybackPosMs = clip.StartTimeMs + clip.DurationMs;
                        }
                    }

                    decoder->RenderCurrentFrame(
                        renderer->GetTargetTexture(),
                        renderer->GetWidth(),
                        renderer->GetHeight());
                }
                else if (targetIdx == -1 && !videoClips.empty() && currentPlaybackPosMs >= (videoClips.back().StartTimeMs + videoClips.back().DurationMs))
                {
                    if (isPlaying.load())
                    {
                        isPlaying.store(false);
                        if (audioPlayer) audioPlayer->Pause();
                    }
                }
            }
            else if (decoder && decoder->IsLoaded())
            {
                if (isPlaying.load())
                {
                    bool gotFrame = decoder->DecodeNextFrame(&currentPlaybackPosMs);
                    if (!gotFrame && decoder->GetDurationMs() > 0 && currentPlaybackPosMs >= decoder->GetDurationMs() - 50)
                    {
                        isPlaying.store(false);
                    }
                }
                decoder->RenderCurrentFrame(
                    renderer->GetTargetTexture(),
                    renderer->GetWidth(),
                    renderer->GetHeight());
            }
        }

        // 3. Google Skia vector overlay rendering (Text, Rounded Rectangles, Circles, Shapes)
        if (skiaRenderer && renderer->GetTargetTexture())
        {
            if (skiaRenderer->BeginDraw(renderer->GetTargetTexture()))
            {
                std::lock_guard<std::mutex> lock(elementsLock);
                for (const auto& kv : elements)
                {
                    const auto& el = kv.second;
                    // Only draw elements active at current playback time
                    if (currentPlaybackPosMs >= el.StartTimeMs && currentPlaybackPosMs <= (el.StartTimeMs + el.DurationMs))
                    {
                        skiaRenderer->DrawPrimitive(el);
                    }
                }
                skiaRenderer->EndDraw();
            }
        }

        renderer->EndFrame();
        renderer->Present();
    }
};

extern "C" {

ENGINE_API void* Engine_Initialize(HWND hWnd, int width, int height)
{
    auto ctx = new (std::nothrow) EngineContext();
    if (!ctx) return nullptr;

    ctx->hWnd = hWnd;
    ctx->width = width > 0 ? width : 1280;
    ctx->height = height > 0 ? height : 720;

    ctx->renderer = std::make_unique<D3D11Renderer>();
    if (!ctx->renderer->Initialize(hWnd, ctx->width, ctx->height))
    {
        delete ctx;
        return nullptr;
    }

    ctx->colorConverter = std::make_unique<VideoColorConverter>();
    ctx->colorConverter->Initialize(
        ctx->renderer->GetDevice(),
        ctx->renderer->GetContext(),
        ctx->width,
        ctx->height);

    ctx->skiaRenderer = std::make_unique<SkiaRenderer>();
    ctx->skiaRenderer->Initialize(ctx->renderer->GetDevice(), ctx->width, ctx->height);

    ctx->decoder = std::make_unique<FFmpegDecoder>();
    ctx->decoder->Initialize(ctx->renderer->GetDevice(), ctx->colorConverter.get());

    ctx->encoder = std::make_unique<FFmpegEncoder>();
    ctx->audioPlayer = std::make_unique<AudioPlayer>();

    // Start background playback thread
    ctx->playbackThread = std::thread([ctx]() {
        while (!ctx->stopPlaybackLoop.load())
        {
            if (ctx->isPlaying.load())
            {
                ctx->RenderSingleFrame();
                std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 fps
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        }
    });

    // Initial render
    ctx->RenderSingleFrame();

    return ctx;
}

ENGINE_API void Engine_Resize(void* engine, int width, int height)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx || !ctx->renderer) return;

    std::lock_guard<std::recursive_mutex> lock(ctx->renderLock);

    ctx->width = width;
    ctx->height = height;
    ctx->renderer->Resize(width, height);
    if (ctx->colorConverter)
    {
        ctx->colorConverter->Resize(width, height);
    }
    if (ctx->skiaRenderer)
    {
        ctx->skiaRenderer->Resize(width, height);
    }
    ctx->RenderSingleFrame();
}

ENGINE_API void Engine_Destroy(void* engine)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx) return;

    // 1. Stop playback thread and join
    ctx->stopPlaybackLoop.store(true);
    if (ctx->playbackThread.joinable())
    {
        ctx->playbackThread.join();
    }

    // 2. Stop and destroy audio player (stops playback thread and joins it)
    if (ctx->audioPlayer)
    {
        ctx->audioPlayer->Stop();
        ctx->audioPlayer.reset();
    }

    // 3. Cancel and destroy encoder (cancels export and joins worker thread)
    if (ctx->encoder)
    {
        ctx->encoder->CancelExport();
        ctx->encoder.reset();
    }

    // 4. Clean up and destroy decoder and renderers under renderLock
    {
        std::lock_guard<std::recursive_mutex> lock(ctx->renderLock);

        if (ctx->decoder)
        {
            ctx->decoder->Cleanup();
            ctx->decoder.reset();
        }
        if (ctx->skiaRenderer)
        {
            ctx->skiaRenderer->Cleanup();
            ctx->skiaRenderer.reset();
        }
        if (ctx->colorConverter)
        {
            ctx->colorConverter->Cleanup();
            ctx->colorConverter.reset();
        }
        if (ctx->renderer)
        {
            ctx->renderer->Cleanup();
            ctx->renderer.reset();
        }
    }

    // 5. Delete context now that all threads are terminated and all subsystems destroyed
    delete ctx;
}

ENGINE_API bool Engine_LoadMedia(void* engine, const wchar_t* filePath)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx || !ctx->decoder) return false;

    bool ok = ctx->decoder->OpenFile(filePath);
    if (ok)
    {
        ctx->currentPlaybackPosMs = 0;
        ctx->currentClipIndex = 0;
        ctx->audioDirty.store(true);
        ctx->RenderSingleFrame();
    }
    return ok;
}

ENGINE_API void Engine_ClearVideoClips(void* engine)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx) return;
    std::lock_guard<std::mutex> lock(ctx->videoClipsLock);
    ctx->videoClips.clear();
    ctx->currentClipIndex = -1;
    ctx->audioDirty.store(true);
}

ENGINE_API void Engine_AddVideoClip(void* engine, const wchar_t* filePath, int64_t startTimeMs, int64_t durationMs)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx || !filePath) return;
    std::lock_guard<std::mutex> lock(ctx->videoClipsLock);
    VideoClipDesc desc = {};
    wcsncpy_s(desc.FilePath, filePath, 259);
    desc.StartTimeMs = startTimeMs;
    desc.DurationMs = durationMs;
    ctx->videoClips.push_back(desc);
    if (ctx->videoClips.size() == 1 && ctx->decoder && ctx->decoder->IsLoaded())
    {
        ctx->currentClipIndex = 0;
    }
    ctx->audioDirty.store(true);
}

ENGINE_API void Engine_ClearAudioClips(void* engine)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx) return;
    std::lock_guard<std::mutex> lock(ctx->audioClipsLock);
    ctx->audioClips.clear();
    ctx->audioDirty.store(true);
}

ENGINE_API void Engine_AddAudioClip(void* engine, const wchar_t* filePath, int64_t startTimeMs, int64_t durationMs)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx || !filePath) return;
    std::lock_guard<std::mutex> lock(ctx->audioClipsLock);
    VideoClipDesc desc = {};
    wcsncpy_s(desc.FilePath, filePath, 259);
    desc.StartTimeMs = startTimeMs;
    desc.DurationMs = durationMs;
    ctx->audioClips.push_back(desc);
    ctx->audioDirty.store(true);
}

ENGINE_API int Engine_ExtractAudioWaveform(const wchar_t* filePath, float* outSamples, int maxSamples)
{
    if (!filePath || !outSamples || maxSamples <= 0) return 0;

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return 0;
    std::string utf8Path(utf8Len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, filePath, -1, &utf8Path[0], utf8Len, nullptr, nullptr);

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, utf8Path.c_str(), nullptr, nullptr) != 0 || !fmtCtx)
    {
        return 0;
    }

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0)
    {
        avformat_close_input(&fmtCtx);
        return 0;
    }

    const AVCodec* decoder = nullptr;
    int audioStreamIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (audioStreamIdx < 0 || !decoder)
    {
        avformat_close_input(&fmtCtx);
        return 0;
    }

    AVStream* audioStream = fmtCtx->streams[audioStreamIdx];
    AVCodecContext* codecCtx = avcodec_alloc_context3(decoder);
    if (!codecCtx)
    {
        avformat_close_input(&fmtCtx);
        return 0;
    }

    if (avcodec_parameters_to_context(codecCtx, audioStream->codecpar) < 0 ||
        avcodec_open2(codecCtx, decoder, nullptr) < 0)
    {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return 0;
    }

    int64_t durMs = 0;
    if (audioStream->duration != AV_NOPTS_VALUE && audioStream->time_base.den > 0)
    {
        durMs = static_cast<int64_t>(audioStream->duration * av_q2d(audioStream->time_base) * 1000.0);
    }
    else if (fmtCtx->duration != AV_NOPTS_VALUE)
    {
        durMs = fmtCtx->duration / (AV_TIME_BASE / 1000);
    }
    if (durMs <= 0) durMs = 10000;

    SwrContext* swrCtx = nullptr;
    AVChannelLayout monoLayout;
    av_channel_layout_default(&monoLayout, 1);
    swr_alloc_set_opts2(&swrCtx, &monoLayout, AV_SAMPLE_FMT_FLT, 8000,
                        &codecCtx->ch_layout, codecCtx->sample_fmt, codecCtx->sample_rate,
                        0, nullptr);
    if (!swrCtx || swr_init(swrCtx) < 0)
    {
        if (swrCtx) swr_free(&swrCtx);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return 0;
    }

    std::vector<float> peaks(maxSamples, 0.05f);
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();

    float resampleBuffer[4096];
    uint8_t* outData[1] = { reinterpret_cast<uint8_t*>(resampleBuffer) };

    while (av_read_frame(fmtCtx, pkt) >= 0)
    {
        if (pkt->stream_index == audioStreamIdx)
        {
            if (avcodec_send_packet(codecCtx, pkt) >= 0)
            {
                while (avcodec_receive_frame(codecCtx, frame) >= 0)
                {
                    int64_t ptsMs = 0;
                    if (frame->pts != AV_NOPTS_VALUE && audioStream->time_base.den > 0)
                    {
                        ptsMs = static_cast<int64_t>(frame->pts * av_q2d(audioStream->time_base) * 1000.0);
                    }
                    int bucket = static_cast<int>((ptsMs * maxSamples) / durMs);
                    if (bucket >= 0 && bucket < maxSamples)
                    {
                        int converted = swr_convert(swrCtx, outData, 4096,
                            const_cast<const uint8_t**>(frame->data), frame->nb_samples);
                        float maxAmp = 0.0f;
                        for (int i = 0; i < converted; ++i)
                        {
                            float v = fabsf(resampleBuffer[i]);
                            if (v > maxAmp) maxAmp = v;
                        }
                        if (maxAmp > peaks[bucket])
                        {
                            peaks[bucket] = maxAmp;
                        }
                    }
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(pkt);
    }

    // Flush decoder
    avcodec_send_packet(codecCtx, nullptr);
    while (avcodec_receive_frame(codecCtx, frame) >= 0)
    {
        av_frame_unref(frame);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swrCtx);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    // Normalize peaks
    float maxVal = 0.01f;
    for (int i = 0; i < maxSamples; ++i)
    {
        if (peaks[i] > maxVal) maxVal = peaks[i];
    }
    for (int i = 0; i < maxSamples; ++i)
    {
        outSamples[i] = (std::clamp)(peaks[i] / maxVal, 0.05f, 1.0f);
    }

    return maxSamples;
}

ENGINE_API int64_t Engine_GetAudioDurationMs(const wchar_t* filePath)
{
    if (!filePath) return 0;
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return 0;
    std::string utf8Path(utf8Len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, filePath, -1, &utf8Path[0], utf8Len, nullptr, nullptr);

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, utf8Path.c_str(), nullptr, nullptr) != 0) return 0;
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0)
    {
        avformat_close_input(&fmtCtx);
        return 0;
    }

    int64_t durMs = 0;
    const AVCodec* decoder = nullptr;
    int audioStreamIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (audioStreamIdx >= 0 && fmtCtx->streams[audioStreamIdx]->duration != AV_NOPTS_VALUE && fmtCtx->streams[audioStreamIdx]->time_base.den > 0)
    {
        durMs = static_cast<int64_t>(fmtCtx->streams[audioStreamIdx]->duration * av_q2d(fmtCtx->streams[audioStreamIdx]->time_base) * 1000.0);
    }
    else if (fmtCtx->duration > 0 && fmtCtx->duration != AV_NOPTS_VALUE)
    {
        durMs = fmtCtx->duration / (AV_TIME_BASE / 1000);
    }
    avformat_close_input(&fmtCtx);
    return durMs;
}

ENGINE_API void Engine_Seek(void* engine, int64_t timeStampMs)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx) return;

    std::lock_guard<std::recursive_mutex> rLock(ctx->renderLock);
    ctx->currentPlaybackPosMs = (std::max)((int64_t)0, timeStampMs);

    {
        std::lock_guard<std::mutex> vLock(ctx->videoClipsLock);
        if (!ctx->videoClips.empty())
        {
            int targetIdx = -1;
            for (size_t i = 0; i < ctx->videoClips.size(); ++i)
            {
                if (ctx->currentPlaybackPosMs >= ctx->videoClips[i].StartTimeMs &&
                    ctx->currentPlaybackPosMs < (ctx->videoClips[i].StartTimeMs + ctx->videoClips[i].DurationMs))
                {
                    targetIdx = static_cast<int>(i);
                    break;
                }
            }
            if (targetIdx == -1 && !ctx->videoClips.empty())
            {
                targetIdx = static_cast<int>(ctx->videoClips.size() - 1);
            }

            if (targetIdx >= 0 && targetIdx < static_cast<int>(ctx->videoClips.size()))
            {
                const auto& clip = ctx->videoClips[targetIdx];
                int64_t localOffset = (std::max)((int64_t)0, ctx->currentPlaybackPosMs - clip.StartTimeMs);
                if (ctx->currentClipIndex != targetIdx)
                {
                    ctx->decoder->OpenFile(clip.FilePath);
                    ctx->currentClipIndex = targetIdx;
                    if (localOffset > 50)
                    {
                        ctx->decoder->Seek(localOffset);
                    }
                }
                else
                {
                    if (localOffset > 50 || ctx->decoder->GetCurrentPositionMs() > 50 || !ctx->decoder->HasCachedFrame())
                    {
                        ctx->decoder->Seek(localOffset);
                    }
                }
            }
        }
        else if (ctx->decoder)
        {
            ctx->decoder->Seek(ctx->currentPlaybackPosMs);
        }
    }

    if (ctx->audioPlayer)
    {
        ctx->audioPlayer->Seek(ctx->currentPlaybackPosMs);
    }

    ctx->RenderSingleFrame();
}

ENGINE_API void Engine_Play(void* engine)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx) return;
    int64_t totalDur = Engine_GetDurationMs(engine);
    if (totalDur > 0 && ctx->currentPlaybackPosMs >= totalDur - 50)
    {
        Engine_Seek(engine, 0);
    }

    if (ctx->audioPlayer)
    {
        if (ctx->audioDirty.load())
        {
            std::vector<VideoClipDesc> effClips;
            {
                std::lock_guard<std::mutex> aLock(ctx->audioClipsLock);
                effClips = ctx->audioClips;
            }
            if (effClips.empty())
            {
                std::lock_guard<std::mutex> vLock(ctx->videoClipsLock);
                effClips = ctx->videoClips;
            }
            ctx->audioPlayer->LoadClips(effClips, totalDur);
            ctx->audioDirty.store(false);
        }
        ctx->audioPlayer->Play(ctx->currentPlaybackPosMs);
    }

    ctx->isPlaying.store(true);
}

ENGINE_API void Engine_Pause(void* engine)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx) return;
    ctx->isPlaying.store(false);
    if (ctx->audioPlayer)
    {
        ctx->audioPlayer->Pause();
    }
}

ENGINE_API bool Engine_IsPlaying(void* engine)
{
    auto ctx = static_cast<EngineContext*>(engine);
    return ctx ? ctx->isPlaying.load() : false;
}

ENGINE_API int64_t Engine_GetDurationMs(void* engine)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx) return 0;
    std::lock_guard<std::mutex> lock(ctx->videoClipsLock);
    if (!ctx->videoClips.empty())
    {
        int64_t total = 0;
        for (const auto& c : ctx->videoClips)
        {
            total = (std::max)(total, c.StartTimeMs + c.DurationMs);
        }
        return total;
    }
    return ctx->decoder ? ctx->decoder->GetDurationMs() : 0;
}

ENGINE_API int64_t Engine_GetLoadedMediaDurationMs(void* engine)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx || !ctx->decoder) return 0;
    return ctx->decoder->GetDurationMs();
}

ENGINE_API int64_t Engine_GetCurrentPositionMs(void* engine)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx) return 0;
    return ctx->currentPlaybackPosMs;
}

ENGINE_API void Engine_GetMediaDimensions(void* engine, int* width, int* height)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx || !ctx->decoder)
    {
        if (width) *width = 0;
        if (height) *height = 0;
        return;
    }
    if (width) *width = ctx->decoder->GetWidth();
    if (height) *height = ctx->decoder->GetHeight();
}

ENGINE_API void Engine_RenderFrame(void* engine)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx) return;
    ctx->RenderSingleFrame();
}

ENGINE_API void Engine_UpsertElement(void* engine, const PrimitiveElementDesc* desc)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx || !desc) return;

    {
        std::lock_guard<std::mutex> lock(ctx->elementsLock);
        ctx->elements[desc->ElementId] = *desc;
    }

    if (!ctx->isPlaying.load())
    {
        ctx->RenderSingleFrame();
    }
}

ENGINE_API void Engine_RemoveElement(void* engine, int elementId)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx) return;

    {
        std::lock_guard<std::mutex> lock(ctx->elementsLock);
        ctx->elements.erase(elementId);
    }

    if (!ctx->isPlaying.load())
    {
        ctx->RenderSingleFrame();
    }
}

ENGINE_API void Engine_ClearElements(void* engine)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx) return;

    {
        std::lock_guard<std::mutex> lock(ctx->elementsLock);
        ctx->elements.clear();
    }

    if (!ctx->isPlaying.load())
    {
        ctx->RenderSingleFrame();
    }
}

ENGINE_API bool Engine_StartExport(
    void* engine, 
    const ExportConfig* config, 
    ExportProgressCallback callback)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx || !ctx->encoder || !config) return false;

    std::vector<PrimitiveElementDesc> elemList;
    {
        std::lock_guard<std::mutex> lock(ctx->elementsLock);
        for (const auto& kv : ctx->elements)
        {
            elemList.push_back(kv.second);
        }
    }

    std::vector<VideoClipDesc> clips;
    {
        std::lock_guard<std::mutex> lock(ctx->videoClipsLock);
        clips = ctx->videoClips;
    }

    std::vector<VideoClipDesc> audioClips;
    {
        std::lock_guard<std::mutex> lock(ctx->audioClipsLock);
        audioClips = ctx->audioClips;
    }

    return ctx->encoder->StartExport(
        ctx->renderer->GetDevice(),
        ctx->colorConverter.get(),
        ctx->skiaRenderer.get(),
        ctx->decoder.get(),
        clips,
        audioClips,
        elemList,
        *config,
        callback,
        ctx->width,
        ctx->height);
}

ENGINE_API void Engine_CancelExport(void* engine)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx || !ctx->encoder) return;
    ctx->encoder->CancelExport();
}

ENGINE_API bool Engine_IsExporting(void* engine)
{
    auto ctx = static_cast<EngineContext*>(engine);
    return ctx && ctx->encoder ? ctx->encoder->IsExporting() : false;
}

ENGINE_API void Engine_EnumerateFonts(FontEnumerationCallback callback, void* userData)
{
    SkiaRenderer::EnumerateFonts(callback, userData);
}

}
