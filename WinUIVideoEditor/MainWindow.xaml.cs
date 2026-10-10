using System;
using System.IO;
using System.Linq;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.Storage.Pickers;
using Windows.UI;
using WinRT.Interop;
using WinUIVideoEditor.Controls;
using WinUIVideoEditor.Models;
using WinUIVideoEditor.Native;
using WinUIVideoEditor.ViewModels;

namespace WinUIVideoEditor
{
    public sealed partial class MainWindow : Window
    {
        private readonly MainViewModel _viewModel = new();
        private IntPtr _hwnd = IntPtr.Zero;
        private bool _isUpdatingUiFromModel = false;

        public MainWindow()
        {
            this.InitializeComponent();

            _hwnd = WindowNative.GetWindowHandle(this);

            var windowId = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(_hwnd);
            var appWindow = AppWindow.GetFromWindowId(windowId);
            appWindow.Title = "WinUI 视频剪辑器";
            appWindow.Resize(new Windows.Graphics.SizeInt32(1440, 900));

            NativeViewport.EngineInitialized += OnEngineInitialized;
            NativeViewport.SetParentHwnd(_hwnd);

            this.Activated += (s, e) =>
            {
                NativeViewport.UpdateChildWindowPosition();
            };

            _viewModel.Timeline.PropertyChanged += (s, e) =>
            {
                if (e.PropertyName == nameof(TimelineModel.CurrentTimeMs))
                {
                    DispatcherQueue.TryEnqueue(() =>
                    {
                        TxtCurrentTime.Text = _viewModel.Timeline.CurrentTimeFormatted;
                    });
                }
                else if (e.PropertyName == nameof(TimelineModel.TotalDurationMs) || e.PropertyName == nameof(TimelineModel.CanPlay))
                {
                    DispatcherQueue.TryEnqueue(() =>
                    {
                        TxtTotalDuration.Text = _viewModel.Timeline.TotalDurationFormatted;
                        BtnPlayPause.IsEnabled = _viewModel.Timeline.CanPlay;
                    });
                }
            };

            _viewModel.PropertyChanged += (s, e) =>
            {
                if (e.PropertyName == nameof(MainViewModel.IsPlaying))
                {
                    DispatcherQueue.TryEnqueue(() =>
                    {
                        IconPlayPause.Glyph = _viewModel.IsPlaying ? "\uE769" : "\uE768"; // Pause : Play
                    });
                }
                else if (e.PropertyName == nameof(MainViewModel.SelectedClip))
                {
                    DispatcherQueue.TryEnqueue(UpdatePropertyPanelFromSelectedClip);
                }
            };
        }

        private void OnEngineInitialized(IntPtr engine)
        {
            _viewModel.SetEngine(engine);
            NativeViewport.BindViewModel(_viewModel);
            ViewportOverlay.BindViewModel(_viewModel);
            TimelineView.BindViewModel(_viewModel);

            CmbFontFamily.ItemsSource = _viewModel.AvailableFonts;
            if (_viewModel.AvailableFonts.Count > 0)
            {
                CmbFontFamily.SelectedIndex = 0;
            }
        }

        private void UpdatePropertyPanelFromSelectedClip()
        {
            var clip = _viewModel.SelectedClip;
            _isUpdatingUiFromModel = true;

            if (clip != null && clip.TrackType != TrackType.Video)
            {
                if (clip.ElementType == 0) // Text
                {
                    LeftToolboxTab.SelectedIndex = 0; // Switch to text tab
                    TbTextContent.Text = clip.ContentText;
                    SliderFontSize.Value = clip.FontSize;
                    TxtFontSize.Text = ((int)clip.FontSize).ToString();
                    ColorPickerFill.Color = clip.FillColor;
                    ColorPickerStroke.Color = clip.StrokeColor;
                    SliderStrokeWidth.Value = clip.StrokeWidth;
                    TxtStrokeWidth.Text = ((int)clip.StrokeWidth).ToString();

                    if (!string.IsNullOrEmpty(clip.FontFamily) && _viewModel.AvailableFonts.Contains(clip.FontFamily))
                    {
                        CmbFontFamily.SelectedItem = clip.FontFamily;
                    }
                }
                else // Shape
                {
                    LeftToolboxTab.SelectedIndex = 1; // Switch to shape tab
                    ShapeColorPickerFill.Color = clip.FillColor;
                    ShapeColorPickerStroke.Color = clip.StrokeColor;
                    SliderShapeStrokeWidth.Value = clip.StrokeWidth;
                    TxtShapeStrokeWidth.Text = ((int)clip.StrokeWidth).ToString();
                    NbShapeWidth.Value = clip.Width;
                    NbShapeHeight.Value = clip.Height;
                }
            }

            _isUpdatingUiFromModel = false;
        }

        private async void OnOpenMediaClicked(object sender, RoutedEventArgs e)
        {
            var picker = new FileOpenPicker();
            picker.SuggestedStartLocation = PickerLocationId.VideosLibrary;
            picker.FileTypeFilter.Add(".mp4");
            picker.FileTypeFilter.Add(".mkv");
            picker.FileTypeFilter.Add(".avi");
            picker.FileTypeFilter.Add(".mov");
            picker.FileTypeFilter.Add(".ts");

            InitializeWithWindow.Initialize(picker, _hwnd);

            var file = await picker.PickSingleFileAsync();
            if (file != null)
            {
                bool ok = _viewModel.LoadMedia(file.Path);
                if (ok)
                {
                    TxtTotalDuration.Text = _viewModel.Timeline.TotalDurationFormatted;
                    TxtCurrentTime.Text = "00:00:00:00";
                    BtnPlayPause.IsEnabled = _viewModel.Timeline.CanPlay;
                    NativeViewport.UpdateChildWindowPosition();
                    if (NativeViewport.EngineHandle != IntPtr.Zero)
                    {
                        NativeEngineApi.Engine_RenderFrame(NativeViewport.EngineHandle);
                    }
                }
            }
        }

        private void OnPlayPauseClicked(object sender, RoutedEventArgs e)
        {
            _viewModel.TogglePlayPause();
        }

        private void OnPreviousFrameClicked(object sender, RoutedEventArgs e)
        {
            long newPos = Math.Max(0, _viewModel.Timeline.CurrentTimeMs - 33);
            _viewModel.SeekTo(newPos);
        }

        private void OnNextFrameClicked(object sender, RoutedEventArgs e)
        {
            long newPos = Math.Min(_viewModel.Timeline.TotalDurationMs, _viewModel.Timeline.CurrentTimeMs + 33);
            _viewModel.SeekTo(newPos);
        }

        private void OnAddTextClicked(object sender, RoutedEventArgs e)
        {
            string initialText = !string.IsNullOrWhiteSpace(TbTextContent.Text) ? TbTextContent.Text : "请输入字幕文本";
            var clip = _viewModel.AddTextClip(initialText);
            LeftToolboxTab.SelectedIndex = 0;
            UpdatePropertyPanelFromSelectedClip();
            ViewportOverlay.Refresh();
        }

        private void OnAddRectangleClicked(object sender, RoutedEventArgs e)
        {
            var clip = _viewModel.AddRectangleClip();
            LeftToolboxTab.SelectedIndex = 1;
            UpdatePropertyPanelFromSelectedClip();
            ViewportOverlay.Refresh();
        }

        private void OnAddCircleClicked(object sender, RoutedEventArgs e)
        {
            var clip = _viewModel.AddCircleClip();
            LeftToolboxTab.SelectedIndex = 1;
            UpdatePropertyPanelFromSelectedClip();
            ViewportOverlay.Refresh();
        }

        private void OnAddTriangleClicked(object sender, RoutedEventArgs e)
        {
            var clip = _viewModel.AddTriangleClip();
            LeftToolboxTab.SelectedIndex = 1;
            UpdatePropertyPanelFromSelectedClip();
            ViewportOverlay.Refresh();
        }

        private void OnAddStarClicked(object sender, RoutedEventArgs e)
        {
            var clip = _viewModel.AddStarClip();
            LeftToolboxTab.SelectedIndex = 1;
            UpdatePropertyPanelFromSelectedClip();
            ViewportOverlay.Refresh();
        }

        private void OnAddArrowClicked(object sender, RoutedEventArgs e)
        {
            var clip = _viewModel.AddArrowClip();
            LeftToolboxTab.SelectedIndex = 1;
            UpdatePropertyPanelFromSelectedClip();
            ViewportOverlay.Refresh();
        }

        private void OnAddDiamondClicked(object sender, RoutedEventArgs e)
        {
            var clip = _viewModel.AddDiamondClip();
            LeftToolboxTab.SelectedIndex = 1;
            UpdatePropertyPanelFromSelectedClip();
            ViewportOverlay.Refresh();
        }

        private void OnTextContentChanged(object sender, TextChangedEventArgs e)
        {
            if (_isUpdatingUiFromModel || _viewModel.SelectedClip == null || _viewModel.SelectedClip.ElementType != 0) return;
            var clip = _viewModel.SelectedClip;
            clip.ContentText = TbTextContent.Text;
            if (!string.IsNullOrWhiteSpace(TbTextContent.Text))
            {
                clip.Title = TbTextContent.Text.Length > 8 ? TbTextContent.Text.Substring(0, 8) + "..." : TbTextContent.Text;
            }
            float estWidth = Math.Max(100, clip.ContentText.Length * clip.FontSize);
            clip.Width = estWidth;
            clip.Height = clip.FontSize * 1.5f;
            _viewModel.SyncClipToEngine(clip);
            if (NativeViewport.EngineHandle != IntPtr.Zero)
            {
                NativeEngineApi.Engine_RenderFrame(NativeViewport.EngineHandle);
            }
            ViewportOverlay.Refresh();
        }

        private void OnFontFamilyChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_isUpdatingUiFromModel || _viewModel.SelectedClip == null || _viewModel.SelectedClip.ElementType != 0) return;
            if (CmbFontFamily.SelectedItem is string font)
            {
                _viewModel.SelectedClip.FontFamily = font;
                _viewModel.SyncClipToEngine(_viewModel.SelectedClip);
                if (NativeViewport.EngineHandle != IntPtr.Zero)
                {
                    NativeEngineApi.Engine_RenderFrame(NativeViewport.EngineHandle);
                }
                ViewportOverlay.Refresh();
            }
        }

        private void OnFontSizeChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
        {
            TxtFontSize.Text = ((int)e.NewValue).ToString();
            if (_isUpdatingUiFromModel || _viewModel.SelectedClip == null || _viewModel.SelectedClip.ElementType != 0) return;
            var clip = _viewModel.SelectedClip;
            clip.FontSize = (float)e.NewValue;
            float estWidth = Math.Max(100, clip.ContentText.Length * clip.FontSize);
            clip.Width = estWidth;
            clip.Height = clip.FontSize * 1.5f;
            _viewModel.SyncClipToEngine(clip);
            if (NativeViewport.EngineHandle != IntPtr.Zero)
            {
                NativeEngineApi.Engine_RenderFrame(NativeViewport.EngineHandle);
            }
            ViewportOverlay.Refresh();
        }

        private void OnFillColorChanged(ColorPicker sender, ColorChangedEventArgs args)
        {
            if (_isUpdatingUiFromModel || _viewModel.SelectedClip == null || _viewModel.SelectedClip.ElementType != 0) return;
            _viewModel.SelectedClip.FillColor = args.NewColor;
            _viewModel.SyncClipToEngine(_viewModel.SelectedClip);
            ViewportOverlay.Refresh();
        }

        private void OnStrokeColorChanged(ColorPicker sender, ColorChangedEventArgs args)
        {
            if (_isUpdatingUiFromModel || _viewModel.SelectedClip == null || _viewModel.SelectedClip.ElementType != 0) return;
            _viewModel.SelectedClip.StrokeColor = args.NewColor;
            _viewModel.SyncClipToEngine(_viewModel.SelectedClip);
            ViewportOverlay.Refresh();
        }

        private void OnStrokeWidthChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
        {
            TxtStrokeWidth.Text = ((int)e.NewValue).ToString();
            if (_isUpdatingUiFromModel || _viewModel.SelectedClip == null || _viewModel.SelectedClip.ElementType != 0) return;
            _viewModel.SelectedClip.StrokeWidth = (float)e.NewValue;
            _viewModel.SyncClipToEngine(_viewModel.SelectedClip);
            ViewportOverlay.Refresh();
        }

        private void OnShapeFillColorChanged(ColorPicker sender, ColorChangedEventArgs args)
        {
            if (_isUpdatingUiFromModel || _viewModel.SelectedClip == null || _viewModel.SelectedClip.ElementType == 0) return;
            _viewModel.SelectedClip.FillColor = args.NewColor;
            _viewModel.SyncClipToEngine(_viewModel.SelectedClip);
            ViewportOverlay.Refresh();
        }

        private void OnShapeStrokeColorChanged(ColorPicker sender, ColorChangedEventArgs args)
        {
            if (_isUpdatingUiFromModel || _viewModel.SelectedClip == null || _viewModel.SelectedClip.ElementType == 0) return;
            _viewModel.SelectedClip.StrokeColor = args.NewColor;
            _viewModel.SyncClipToEngine(_viewModel.SelectedClip);
            ViewportOverlay.Refresh();
        }

        private void OnShapeStrokeWidthChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
        {
            TxtShapeStrokeWidth.Text = ((int)e.NewValue).ToString();
            if (_isUpdatingUiFromModel || _viewModel.SelectedClip == null || _viewModel.SelectedClip.ElementType == 0) return;
            _viewModel.SelectedClip.StrokeWidth = (float)e.NewValue;
            _viewModel.SyncClipToEngine(_viewModel.SelectedClip);
            ViewportOverlay.Refresh();
        }

        private void OnShapeDimensionChanged(NumberBox sender, NumberBoxValueChangedEventArgs args)
        {
            if (_isUpdatingUiFromModel || _viewModel.SelectedClip == null) return;
            if (sender == NbShapeWidth && !double.IsNaN(NbShapeWidth.Value))
            {
                _viewModel.SelectedClip.Width = (float)Math.Max(10, NbShapeWidth.Value);
            }
            else if (sender == NbShapeHeight && !double.IsNaN(NbShapeHeight.Value))
            {
                _viewModel.SelectedClip.Height = (float)Math.Max(10, NbShapeHeight.Value);
            }
            _viewModel.SyncClipToEngine(_viewModel.SelectedClip);
            ViewportOverlay.Refresh();
        }

        private void OnDeleteElementClicked(object sender, RoutedEventArgs e)
        {
            _viewModel.DeleteSelectedClip();
            UpdatePropertyPanelFromSelectedClip();
            ViewportOverlay.Refresh();
        }

        private void OnExportVideoClicked(object sender, RoutedEventArgs e)
        {
            if (NativeViewport.EngineHandle == IntPtr.Zero) return;

            var window = new ExportWindow(NativeViewport.EngineHandle, _hwnd);
            window.Activate();
        }

        private void OnProjectSettingsClicked(object sender, RoutedEventArgs e)
        {
            var window = new ProjectSettingsWindow(_hwnd, _viewModel.MediaFilePath, _viewModel.Timeline.TotalDurationFormatted);
            window.Activate();
        }
    }
}
