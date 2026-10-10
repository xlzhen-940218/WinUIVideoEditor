# 《多功能视频剪辑器》软件需求规格与架构设计文档 (PRD & Technical Spec)

---

## 1. 项目概述与设计目标

### 1.1 项目定位

本项目旨在基于 **WinUI 3 (Windows App SDK)** 构建一款现代化、高性能、硬件加速驱动的 Windows 桌面级视频剪辑软件。核心渲染管线采用 **Direct3D 11 + Google Skia** 深度整合模式，全程维持显存常驻（Zero-Copy），消除 CPU 与 GPU 之间的大图拷贝损耗，兼顾丰富的 2D 矢量文字排版、形状绘制能力与顶尖的实时回放/导出吞吐量。

### 1.2 兼容性矩阵

* **操作系统**：Windows 10 Version 1609 (Build 14393) 及以上，完整支持 Windows 11。
* **硬件平台**：全面适配 NVIDIA、AMD、Intel 全系独立显卡与集成显卡（Intel UHD/Iris Xe/Arc、AMD Radeon/Vega/APU、NVIDIA GeForce/RTX/GTX）。
* **UI 运行框架**：WinUI 3 (Windows App SDK 1.5+)，使用 C# 作为上层 UI/业务交互语言。
* **底层引擎语言**：C++20 (MSVC v143+)，构建为核心 Native 动态库 (`EngineCore.dll`)。

---

## 2. 系统核心架构与零拷贝数据流规范

```text
┌────────────────────────────────────────────────────────────────────────┐
│                        C# WinUI 3 前端进程 / 交互层                     │
│  - 轨道管理器 (TimelineTrack)       - 画布宿主 (Win32 HwndHost / SwapChain)│
│  - 属性编辑面板 (PropertyPanel)     - 导出控制弹窗 (ExportDialog)          │
└────────────────────────────────────┬───────────────────────────────────┘
                                     │ P/Invoke (严格 C ABI 契约)
┌────────────────────────────────────▼───────────────────────────────────┐
│                    C++ 原生多媒体与渲染引擎 (EngineCore.dll)             │
│                                                                        │
│  [FFmpeg D3D11VA 硬解]                                                 │
│          │ (零拷贝输出 AV_PIX_FMT_D3D11 -> ID3D11Texture2D [NV12])     │
│          ▼                                                             │
│  [D3D11 VideoProcessor / CS] ──► (显存内部 NV12 转 RGBA8)              │
│          │                                                             │
│          ▼                                                             │
│  [中间合成纹理 Target Texture (RGBA8)] ◄──┐                            │
│          │                                │ (显存共享 / 表面包装)       │
│          ▼                                │                            │
│  [Google Skia 渲染器 (GrDirectContext)] ──┘                            │
│  - 文本/字幕排版 (SkParagraph)                                         │
│  - 矢量图形绘制 (矩形、圆形、描边、填充)                                 │
│  - 关键帧变换矩阵 (SkCanvas::concat)                                   │
│          │                                                             │
│          ├─────────────────────────────┬───────────────────────────────┤
│          ▼ (实时预览分支)               ▼ (文件导出分支)                │
│  [DXGI SwapChainPresent]        [D3D11 VideoProcessor / CS]           │
│  - 呈现至 WinUI 3 指定窗口句柄      - RGBA8 转 NV12 (显存内)            │
│    (HWND / SwapChainPanel)             ▼                               │
│                                 [FFmpeg 硬件编码器 (NVENC/AMF/QSV)]     │
│                                 - 输入 AV_PIX_FMT_D3D11 (零内存拷贝)   │
│                                 - 封装输出 MP4 容器                    │
└────────────────────────────────────────────────────────────────────────┘

```

### 2.1 零拷贝 (Zero-Copy) 强制技术约束

1. **禁止内存回读**：全链路严禁任何 `av_image_copy`、`D3D11_MAP_READ` 或将画面读取到系统 RAM (`CPU Memory`) 的操作。
2. **设备拓扑对齐**：引擎启动时通过 `IDXGIFactory1::EnumAdapters1` 枚举最优物理适配器。FFmpeg `AVHWDeviceContext`、D3D11 渲染设备与 SwapChain **必须绑定同一物理 GPU**，规避双显卡设备漂移导致的句柄跨总线拷贝。
3. **多线程互斥安全**：对共享 `ID3D11DeviceContext` 启用 `ID3D11Multithread::SetMultithreadProtected(TRUE)`。

---

## 3. UI 界面布局与交互系统规格

主界面采用 WinUI 3 `Grid` 布局，结合 `GridSplitter` 实现区域拖拽伸缩。

```text
+-----------------------+-----------------------------------------------+
|                       |                                               |
|  功能区 (功能导航/属性) |           画布预览区 (HWND 宿主窗口)           |
|  默认宽度: 1/6 (16.6%) |                                               |
|                       |                                               |
|                       +-----------------------------------------------+
|                       |===== 横向分割条 (GridSplitter) ================|
|                       +-----------------------------------------------+
|                       |                                               |
|                       |           多轨道编辑区 (Timeline)              |
|                       |           默认高度: 1/4 (25.0%)               |
+-----------------------+-----------------------------------------------+
|== 竖向分割条 (Splitter)|

```

### 3.1 布局分区规范

* **左侧功能区 (`LeftPane`)**：
* **默认比例**：主窗口宽度的 $1/6$；最小限制宽度 `200px`。
* **右边缘**：放置纵向 `GridSplitter`，支持鼠标拖拽实时调节宽度。
* **功能 Tab**：
* **文本工具箱**：添加字幕、选择字体族（DirectWrite 枚举）、字号、字体颜色（拾色器）、边框颜色与描边粗细。
* **图形工具箱**：圆形、矩形，支持设置填充色、描边粗细、圆角半径。




* **右侧上方画布区 (`PreviewHost`)**：
* **实现机制**：通过托管独立的纯 Win32 子窗口并挂载到 WinUI 3 容器内，或者利用 WinUI 3 的原生 `SwapChainPanel` 接收底层的渲染输出。
* **交互控制**：画布中渲染的元素支持边界框（Bounding Box）手柄，允许直接在视口中点击选定、拖动位移、等比缩放、改变矩形宽高或圆形半径。


* **右侧下方轨道区 (`TimelinePane`)**：
* **默认比例**：主窗口高度的 $1/4$；最小限制高度 `180px`。
* **顶边缘**：放置横向 `GridSplitter`，支持拖拽调节轨道区高度。
* **时间轴结构**：
* 顶部时间标尺（刻度支持缩放与时间显示格式 `HH:mm:ss:ff`）。
* 时间指针（Playhead/Scrubber），支持拖拽与跳帧。
* 轨道分层：视频主轨、文字轨、图形覆层轨。




* **右上角工具栏 (`TopBar`)**：
* 包含工程设置、播放/暂停控制以及醒目的“导出视频” (AccentButton) 按钮。



### 3.2 轨道与画布双向同步规则

1. **添加响应**：点击左侧添加文本/图形时，在当前时间指针位置就近插入一个默认持续时间（如 3 秒）的 Clip，并自动生成对应的图元对象。
2. **时间轴同步**：点击轨道区上的任何图元片段（Clip）：
* 时间指针自动跳转（Seek）至该 Clip 的入点（Start Time）。
* 画布区立即刷新，显示并高亮选定该元素的控制外框。
* 左侧属性面板自动切换并加载该图元的当前属性（文字、字号、颜色、尺寸）。


3. **画布反选同步**：在画布视口中点击某图元，轨道区对应的 Clip 自动进入高亮选中状态。

---

## 4. 核心功能模块规格说明

### 4.1 视频导入与零拷贝解码模块

* **接口协议**：C++ 导出 `Engine_LoadMediaFile(const wchar_t* path)`。
* **硬件上下文绑定**：
```cpp
// 必须初始化 D3D11VA 硬件设备上下文并绑定到全局 D3D11 设备
av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);

```


* **解码流程**：
* 使用 `avcodec_receive_frame` 提取帧，强制过滤 `AV_PIX_FMT_D3D11`。
* 提取 `frame->data[0]` (转换为 `ID3D11Texture2D*`) 与 `(intptr_t)frame->data[1]` (Subresource 索引)。
* 通过 Direct3D 11 `ID3D11VideoProcessor` 硬件管线直接将 NV12 转换到中间合成 RGBA8 纹理，禁止 CPU 内存参与。



### 4.2 Skia 2D 图形与排版合成引擎

* **上下文初始化**：基于当前 D3D 设备初始化 Skia 引擎（推荐配置 Direct3D / Vulkan 后端），通过包装外部 D3D11 共享资源或直接使用 `GrDirectContext` 操作 RGBA8 目标表面 (`SkSurface`)。
* **绘制流程**：
1. 每一帧刷新时，先由 D3D11 填充解码后的视频底图。
2. 获取绑定的 `SkCanvas`。
3. 读取处于当前时间戳的所有轨道图元（文本、矩形、圆形）。
4. 应用平移、旋转、缩放变换矩阵：`canvas->concat(matrix)`。
5. 绘制图形 (`canvas->drawPath`, `canvas->drawRRect`, `canvas->drawCircle`)。
6. 调用 `SkParagraph` 绘制富文本、描边外框及渐变文字。
7. 执行 `grContext->flushAndSubmit()`，保证渲染结果同步提交给 GPU。



### 4.3 显卡直通硬件编码与导出模块

* **导出弹窗界面规范**：
* 点击右上角“导出”按钮，弹出 WinUI 3 `ContentDialog`。
* **参数配置项**：
* 导出分辨率（默认原始视频分辨率，支持 1080P/4K 等下拉预设）。
* 目标帧率（30 / 60 fps）。
* 编码器选择：根据当前物理硬件自动选中（如 `h264_nvenc`, `h264_amf`, `h264_qsv`），保留软件编码 `libx264` 作为极端情况下的回退选项。
* 码率模式（CBR / VBR）及目标码率设定。
* 保存文件路径选择器。


* **执行控制**：
* 点击“确认导出”后，对话框进入不可取消状态（或提供“中止”按钮）。
* 弹出框内部展示 WinUI 3 `ProgressBar`（支持动态切换确定性进度与百分比文本）及当前帧数/估算剩余时间（ETA）。




* **导出直通流程**：
1. 后台线程按时间轴步进帧（离屏快进 Seek 与渲染）。
2. 每一帧通过 Skia 与 D3D 完成 RGBA8 合成。
3. 通过 `ID3D11VideoProcessor` 在显存中就地将 RGBA8 转换为 NV12 纹理。
4. 分配 `AV_PIX_FMT_D3D11` 的硬件 `AVFrame`，直接将 NV12 纹理填入 `frame->data[0]`。
5. 将硬件帧投递至对应硬件编码器 (`avcodec_send_frame`)，获取 Packet 写入封装，全程 CPU 零拷贝。
6. 导出线程通过 Native 回调向 C# 发送进度百分比（$0 \sim 100\%$），更新 UI。



---

## 5. C# 与 C++ 底层 P/Invoke 接口契约定义

引擎统一采用 C 形式 ABI 导出，防止 C++ Name Mangling 问题：

```csharp
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
    public static extern void Engine_Seek(IntPtr engine, long timeStampMs);

    // 3. 图元添加与属性更新 (结构体序列化或字段同步)
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
    }

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Engine_UpsertElement(IntPtr engine, ref PrimitiveElementDesc desc);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Engine_RemoveElement(IntPtr engine, int elementId);

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
}

```

---

## 6. 实现路线图与 Agent 执行分工 (Milestones)

为便于代码生成 Agent 循序渐进地实现并自动化集成，将工程拆解为以下阶段：

### 阶段一：C++ Native 核心管线与测试验证

1. 搭建 Direct3D 11 初始化代码，绑定 `ID3D11Multithread` 并创建通用上下文。
2. 集成 FFmpeg，实现硬解初始化，验证 `AV_PIX_FMT_D3D11` 的捕获与显示。
3. 链接 Google Skia，完成 `GrDirectContext` 和 `SkSurface` 绑定，实现在 D3D 纹理上叠加绘制矩形、圆形与 `SkParagraph` 文本。
4. 集成硬件编码流程，验证从合成纹理经过格式转换零拷贝推流到 FFmpeg 编码器，并能成功输出有效 MP4 文件。

### 阶段二：WinUI 3 界面与布局框架搭建

1. 创建 WinUI 3 打包应用，构建主窗口三栏式 Grid 架构（左功能区、右上画布、右下轨道）。
2. 配置纵横两条 `GridSplitter`，设置默认尺寸比例（宽度 $1/6$、高度 $1/4$）及边界限制。
3. 实现 Win32 子窗口宿主控件（或者包装 `SwapChainPanel`），将渲染视口句柄注入给底层。

### 阶段三：轨道交互与时间轴双向绑定

1. 构建 C# 数据模型：`TimelineModel`, `TrackModel`, `ClipModel`（关联文字/图形描述数据）。
2. 实现轨道列表控件与时间指针拖动算法。
3. 绑定左侧面板输入项，打通“左侧添加 $\rightarrow$ 轨道生成 Clip $\rightarrow$ 画布更新”以及“点击轨道 Clip $\rightarrow$ 时间轴 Seek $\rightarrow$ 画布选中”双向响应链路。

### 阶段四：导出对话框与生产化调优

1. 实现右上角导出按钮与 `ContentDialog` 进度条交互。
2. 接入后台导出线程与 `ExportProgressCallback` 委托，实时推送百分比并处理异常。
3. 对齐 NVIDIA、AMD、Intel 各家在 NV12 步长对齐与双显卡下的设备枚举保护，确保整机稳定性。
