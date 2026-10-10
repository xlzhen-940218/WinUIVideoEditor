#include "EngineCore.h"
#include "D3D11Renderer.h"
#include "VideoProcessor.h"
#include "SkiaRenderer.h"
#include "FFmpegDecoder.h"
#include "FFmpegEncoder.h"

#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

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

    std::map<int, PrimitiveElementDesc> elements;
    std::mutex elementsLock;
    std::recursive_mutex renderLock;

    std::atomic<bool> isPlaying{ false };
    std::atomic<bool> stopPlaybackLoop{ false };
    std::thread playbackThread;

    int64_t currentPlaybackPosMs = 0;

    void RenderSingleFrame()
    {
        std::lock_guard<std::recursive_mutex> lock(renderLock);
        if (!renderer) return;

        renderer->BeginFrame();

        // 1. Hardware video frame rendering (NV12 -> RGBA8 in VRAM)
        if (decoder && decoder->IsLoaded())
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

        // 2. Google Skia vector overlay rendering (Text, Rounded Rectangles, Circles)
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

    ctx->stopPlaybackLoop.store(true);
    if (ctx->playbackThread.joinable())
    {
        ctx->playbackThread.join();
    }

    std::lock_guard<std::recursive_mutex> lock(ctx->renderLock);

    if (ctx->encoder) ctx->encoder->CancelExport();
    if (ctx->decoder) ctx->decoder->Cleanup();
    if (ctx->skiaRenderer) ctx->skiaRenderer->Cleanup();
    if (ctx->colorConverter) ctx->colorConverter->Cleanup();
    if (ctx->renderer) ctx->renderer->Cleanup();

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
        ctx->RenderSingleFrame();
    }
    return ok;
}

ENGINE_API void Engine_Seek(void* engine, int64_t timeStampMs)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx) return;

    if (ctx->decoder)
    {
        ctx->decoder->Seek(timeStampMs);
    }
    ctx->currentPlaybackPosMs = timeStampMs;
    ctx->RenderSingleFrame();
}

ENGINE_API void Engine_Play(void* engine)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx) return;
    if (ctx->decoder && ctx->decoder->GetDurationMs() > 0 &&
        ctx->currentPlaybackPosMs >= ctx->decoder->GetDurationMs() - 50)
    {
        ctx->decoder->Seek(0);
        ctx->currentPlaybackPosMs = 0;
    }
    ctx->isPlaying.store(true);
}

ENGINE_API void Engine_Pause(void* engine)
{
    auto ctx = static_cast<EngineContext*>(engine);
    if (!ctx) return;
    ctx->isPlaying.store(false);
}

ENGINE_API bool Engine_IsPlaying(void* engine)
{
    auto ctx = static_cast<EngineContext*>(engine);
    return ctx ? ctx->isPlaying.load() : false;
}

ENGINE_API int64_t Engine_GetDurationMs(void* engine)
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

    return ctx->encoder->StartExport(
        ctx->renderer->GetDevice(),
        ctx->colorConverter.get(),
        ctx->skiaRenderer.get(),
        ctx->decoder.get(),
        elemList,
        *config,
        callback);
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
