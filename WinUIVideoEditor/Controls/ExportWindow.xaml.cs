using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Storage.Pickers;
using WinRT.Interop;
using WinUIVideoEditor.Native;

namespace WinUIVideoEditor.Controls
{
    public sealed partial class ExportWindow : Window
    {
        private readonly IntPtr _engine;
        private readonly IntPtr _parentHwnd;
        private readonly IntPtr _hwnd;
        private readonly AppWindow _appWindow;
        private readonly DispatcherQueue _dispatcherQueue;

        private Stopwatch? _stopwatch;
        private bool _isExporting = false;
        private NativeEngineApi.ExportProgressCallback? _progressCallback;

        [StructLayout(LayoutKind.Sequential)]
        private struct RECT
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;
        }

        [DllImport("user32.dll")]
        private static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr SetWindowLongPtr(IntPtr hWnd, int nIndex, IntPtr dwNewLong);
        private const int GWLP_HWNDPARENT = -8;

        public ExportWindow(IntPtr engine, IntPtr parentHwnd)
        {
            this.InitializeComponent();
            _engine = engine;
            _parentHwnd = parentHwnd;
            _dispatcherQueue = DispatcherQueue.GetForCurrentThread();

            _hwnd = WindowNative.GetWindowHandle(this);
            var windowId = Win32Interop.GetWindowIdFromWindow(_hwnd);
            _appWindow = AppWindow.GetFromWindowId(windowId);
            _appWindow.Title = "导出视频";

            int winW = 540;
            int winH = 560;
            _appWindow.Resize(new Windows.Graphics.SizeInt32(winW, winH));

            if (_parentHwnd != IntPtr.Zero)
            {
                SetWindowLongPtr(_hwnd, GWLP_HWNDPARENT, _parentHwnd);
                if (GetWindowRect(_parentHwnd, out RECT parentRect))
                {
                    int centerX = (parentRect.Left + parentRect.Right) / 2;
                    int centerY = (parentRect.Top + parentRect.Bottom) / 2;
                    int posX = Math.Max(50, centerX - (winW / 2));
                    int posY = Math.Max(50, centerY - (winH / 2));
                    _appWindow.Move(new Windows.Graphics.PointInt32(posX, posY));
                }
            }

            // Set default output file path
            string videosFolder = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
            if (string.IsNullOrEmpty(videosFolder)) videosFolder = AppContext.BaseDirectory;
            TbOutputPath.Text = Path.Combine(videosFolder, $"Output_{DateTime.Now:yyyyMMdd_HHmmss}.mp4");
        }

        private async void OnBrowseOutputClicked(object sender, RoutedEventArgs e)
        {
            var savePicker = new FileSavePicker();
            savePicker.SuggestedStartLocation = PickerLocationId.VideosLibrary;
            savePicker.FileTypeChoices.Add("MP4 视频文件", new[] { ".mp4" });
            savePicker.SuggestedFileName = $"Output_{DateTime.Now:yyyyMMdd_HHmmss}";

            InitializeWithWindow.Initialize(savePicker, _hwnd);

            var file = await savePicker.PickSaveFileAsync();
            if (file != null)
            {
                TbOutputPath.Text = file.Path;
            }
        }

        private void OnStartExportClicked(object sender, RoutedEventArgs e)
        {
            if (_isExporting) return;

            string outputPath = TbOutputPath.Text.Trim();
            if (string.IsNullOrWhiteSpace(outputPath)) return;

            StartExportProcess(outputPath);
        }

        private void OnCloseClicked(object sender, RoutedEventArgs e)
        {
            if (_isExporting)
            {
                if (_engine != IntPtr.Zero)
                {
                    NativeEngineApi.Engine_CancelExport(_engine);
                }
            }
            this.Close();
        }

        private void StartExportProcess(string outputPath)
        {
            _isExporting = true;
            BtnStartExport.IsEnabled = false;
            BtnClose.Visibility = Visibility.Collapsed;

            ConfigSection.Visibility = Visibility.Collapsed;
            ProgressSection.Visibility = Visibility.Visible;
            TxtExportStatus.Text = "正在进行 GPU 显存直通硬件编码...";
            PbExport.Value = 0;
            TxtProgressPercent.Text = "0.0%";
            TxtEta.Text = "估算剩余时间: 计算中...";

            _stopwatch = Stopwatch.StartNew();

            // Resolution
            int width = 1920;
            int height = 1080;
            if (CmbResolution.SelectedItem is ComboBoxItem itemRes && itemRes.Tag is string resTag)
            {
                var parts = resTag.Split('x');
                if (parts.Length == 2 && int.TryParse(parts[0], out int w) && int.TryParse(parts[1], out int h))
                {
                    width = w;
                    height = h;
                }
            }

            if (width == 0 || height == 0)
            {
                NativeEngineApi.Engine_GetMediaDimensions(_engine, out width, out height);
                if (width <= 0) width = 1920;
                if (height <= 0) height = 1080;
            }

            // FPS
            int fps = 30;
            if (CmbFps.SelectedItem is ComboBoxItem itemFps && itemFps.Tag is string fpsTag && int.TryParse(fpsTag, out int f))
            {
                fps = f;
            }

            // Encoder
            int encoderType = 0; // Auto
            if (CmbEncoder.SelectedItem is ComboBoxItem itemEnc && itemEnc.Tag is string encTag && int.TryParse(encTag, out int enc))
            {
                encoderType = enc;
            }

            // Bitrate
            int bitrate = 10000000;
            if (CmbBitrate.SelectedItem is ComboBoxItem itemBr && itemBr.Tag is string brTag && int.TryParse(brTag, out int br))
            {
                bitrate = br;
            }

            var config = new NativeEngineApi.ExportConfig
            {
                OutputPath = outputPath,
                Width = width,
                Height = height,
                Fps = fps,
                Bitrate = bitrate,
                HardwareEncoderType = encoderType
            };

            _progressCallback = (float progress, bool isFinished, int errorCode) =>
            {
                _dispatcherQueue.TryEnqueue(() =>
                {
                    if (isFinished)
                    {
                        _isExporting = false;
                        _stopwatch?.Stop();

                        if (errorCode == 0)
                        {
                            TxtExportStatus.Text = "✓ 导出成功完成！";
                            PbExport.Value = 100;
                            TxtProgressPercent.Text = "100.0%";
                            TxtEta.Text = $"总用时: {_stopwatch?.Elapsed.ToString(@"mm\:ss")}";
                        }
                        else if (errorCode == -99)
                        {
                            TxtExportStatus.Text = "导出已中止。";
                        }
                        else
                        {
                            TxtExportStatus.Text = $"导出失败，错误代码: {errorCode}";
                        }

                        BtnCancelExport.Visibility = Visibility.Collapsed;
                        BtnClose.Content = "完成";
                        BtnClose.Visibility = Visibility.Visible;
                    }
                    else
                    {
                        double percent = Math.Clamp(progress * 100.0, 0, 100);
                        PbExport.Value = percent;
                        TxtProgressPercent.Text = $"{percent:F1}%";

                        if (_stopwatch != null && progress > 0.01f)
                        {
                            double elapsedSec = _stopwatch.Elapsed.TotalSeconds;
                            double totalEstSec = elapsedSec / progress;
                            double remainingSec = Math.Max(0, totalEstSec - elapsedSec);
                            TimeSpan remSpan = TimeSpan.FromSeconds(remainingSec);
                            TxtEta.Text = $"估算剩余: {remSpan:mm\\:ss}";
                        }
                    }
                });
            };

            bool started = NativeEngineApi.Engine_StartExport(_engine, ref config, _progressCallback);
            if (!started)
            {
                TxtExportStatus.Text = "导出启动失败，请检查参数设置。";
                BtnClose.Content = "关闭";
                BtnClose.Visibility = Visibility.Visible;
            }
        }

        private void OnCancelExportClicked(object sender, RoutedEventArgs e)
        {
            if (_engine != IntPtr.Zero)
            {
                NativeEngineApi.Engine_CancelExport(_engine);
                TxtExportStatus.Text = "正在中止导出...";
                BtnCancelExport.IsEnabled = false;
            }
        }
    }
}
