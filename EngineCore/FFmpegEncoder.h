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
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <functional>
#include <cstdint>

#include "EngineCore.h"
#include "VideoProcessor.h"
#include "SkiaRenderer.h"
#include "FFmpegDecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

class FFmpegEncoder
{
public:
    FFmpegEncoder();
    ~FFmpegEncoder();

    bool StartExport(
        ID3D11Device* device,
        VideoColorConverter* colorConverter,
        SkiaRenderer* skiaRenderer,
        FFmpegDecoder* decoder,
        const std::vector<PrimitiveElementDesc>& elements,
        const ExportConfig& config,
        ExportProgressCallback callback);

    void CancelExport();
    bool IsExporting() const { return m_isExporting.load(); }

private:
    void ExportWorker(
        ID3D11Device* device,
        VideoColorConverter* colorConverter,
        SkiaRenderer* skiaRenderer,
        FFmpegDecoder* decoder,
        std::vector<PrimitiveElementDesc> elements,
        ExportConfig config,
        ExportProgressCallback callback);

    const AVCodec* SelectEncoder(int hardwareEncoderType);

    std::atomic<bool> m_isExporting{ false };
    std::atomic<bool> m_cancelRequested{ false };
    std::thread m_workerThread;
};
