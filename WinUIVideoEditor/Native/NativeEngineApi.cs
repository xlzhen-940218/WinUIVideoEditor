using System;
using System.Runtime.InteropServices;

namespace WinUIVideoEditor.Native
{
    public static class NativeEngineApi
    {
        private const string DllName = "EngineCore.dll";

        // 1. 引擎生命周期与视口绑定
        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr Engine_Initialize(IntPtr hWnd, int width, int height);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Engine_Resize(IntPtr engine, int width, int height);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Engine_Destroy(IntPtr engine);

        // 2. 媒体流与播放控制
        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        public static extern bool Engine_LoadMedia(IntPtr engine, string filePath);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Engine_ClearVideoClips(IntPtr engine);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        public static extern void Engine_AddVideoClip(IntPtr engine, string filePath, long startTimeMs, long durationMs);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Engine_Seek(IntPtr engine, long timeStampMs);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Engine_Play(IntPtr engine);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Engine_Pause(IntPtr engine);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern bool Engine_IsPlaying(IntPtr engine);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern long Engine_GetDurationMs(IntPtr engine);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern long Engine_GetLoadedMediaDurationMs(IntPtr engine);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern long Engine_GetCurrentPositionMs(IntPtr engine);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Engine_GetMediaDimensions(IntPtr engine, out int width, out int height);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Engine_RenderFrame(IntPtr engine);

        // 3. 图元添加与属性更新
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct PrimitiveElementDesc
        {
            public int ElementId;
            public int ElementType; // 0: Text, 1: Rectangle, 2: Circle
            public float X;
            public float Y;
            public float Width;
            public float Height;
            public uint FillColor;   // 0xAARRGGBB
            public uint StrokeColor; // 0xAARRGGBB
            public float StrokeWidth;
            public float FontSize;

            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
            public string FontFamily;

            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
            public string ContentText;

            public long StartTimeMs;
            public long DurationMs;
            public int IsSelected;
        }

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Engine_UpsertElement(IntPtr engine, ref PrimitiveElementDesc desc);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Engine_RemoveElement(IntPtr engine, int elementId);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Engine_ClearElements(IntPtr engine);

        // 4. 导出控制与进度回调
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void ExportProgressCallback(float progress, bool isFinished, int errorCode);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct ExportConfig
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
            public string OutputPath;
            public int Width;
            public int Height;
            public int Fps;
            public int Bitrate;
            public int HardwareEncoderType; // 0: Auto, 1: NVENC, 2: AMF, 3: QSV
        }

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern bool Engine_StartExport(
            IntPtr engine, 
            ref ExportConfig config, 
            ExportProgressCallback callback);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Engine_CancelExport(IntPtr engine);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern bool Engine_IsExporting(IntPtr engine);

        // 5. 字体枚举
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        public delegate void FontEnumerationCallback(string fontName, IntPtr userData);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Engine_EnumerateFonts(FontEnumerationCallback callback, IntPtr userData);
    }
}
