#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>

#ifdef ENGINECORE_EXPORTS
#define ENGINE_API __declspec(dllexport)
#else
#define ENGINE_API __declspec(dllimport)
#endif

// 进度与枚举回调函数指针类型
typedef void (__cdecl *ExportProgressCallback)(float progress, bool isFinished, int errorCode);
typedef void (__cdecl *FontEnumerationCallback)(const wchar_t* fontName, void* userData);

#pragma pack(push, 8)
struct PrimitiveElementDesc
{
    int ElementId;
    int ElementType; // 0: Text, 1: Rectangle, 2: Circle
    float X;
    float Y;
    float Width;
    float Height;
    uint32_t FillColor;   // 0xAARRGGBB
    uint32_t StrokeColor; // 0xAARRGGBB
    float StrokeWidth;
    float FontSize;
    wchar_t FontFamily[64];
    wchar_t ContentText[256];
    int64_t StartTimeMs;
    int64_t DurationMs;
    int IsSelected; // 0: unselected, 1: selected
};

struct ExportConfig
{
    wchar_t OutputPath[260];
    int Width;
    int Height;
    int Fps;
    int Bitrate;
    int HardwareEncoderType; // 0: Auto, 1: NVENC, 2: AMF, 3: QSV
};
#pragma pack(pop)

extern "C" {

    // 1. 引擎生命周期与视口绑定
    ENGINE_API void* Engine_Initialize(HWND hWnd, int width, int height);
    ENGINE_API void Engine_Resize(void* engine, int width, int height);
    ENGINE_API void Engine_Destroy(void* engine);

    // 2. 媒体流与播放控制
    ENGINE_API bool Engine_LoadMedia(void* engine, const wchar_t* filePath);
    ENGINE_API void Engine_Seek(void* engine, int64_t timeStampMs);
    ENGINE_API void Engine_Play(void* engine);
    ENGINE_API void Engine_Pause(void* engine);
    ENGINE_API bool Engine_IsPlaying(void* engine);
    ENGINE_API int64_t Engine_GetDurationMs(void* engine);
    ENGINE_API int64_t Engine_GetCurrentPositionMs(void* engine);
    ENGINE_API void Engine_GetMediaDimensions(void* engine, int* width, int* height);
    ENGINE_API void Engine_RenderFrame(void* engine);

    // 3. 图元添加与属性更新
    ENGINE_API void Engine_UpsertElement(void* engine, const PrimitiveElementDesc* desc);
    ENGINE_API void Engine_RemoveElement(void* engine, int elementId);
    ENGINE_API void Engine_ClearElements(void* engine);

    // 4. 导出控制与进度回调
    ENGINE_API bool Engine_StartExport(
        void* engine, 
        const ExportConfig* config, 
        ExportProgressCallback callback);

    ENGINE_API void Engine_CancelExport(void* engine);
    ENGINE_API bool Engine_IsExporting(void* engine);

    // 5. DirectWrite 字体枚举辅助
    ENGINE_API void Engine_EnumerateFonts(FontEnumerationCallback callback, void* userData);
}
