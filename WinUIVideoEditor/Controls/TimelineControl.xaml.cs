using System;
using System.Collections.Specialized;
using System.Linq;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using WinUIVideoEditor.Models;
using WinUIVideoEditor.Native;
using WinUIVideoEditor.ViewModels;

namespace WinUIVideoEditor.Controls
{
    public sealed partial class TimelineControl : UserControl
    {
        private MainViewModel? _viewModel;
        private bool _isDraggingRuler = false;

        public TimelineControl()
        {
            this.InitializeComponent();
            SizeChanged += (s, e) => RedrawTimeline();
        }

        public void BindViewModel(MainViewModel viewModel)
        {
            _viewModel = viewModel;
            _viewModel.Timeline.PropertyChanged += (s, e) =>
            {
                if (e.PropertyName == nameof(TimelineModel.CurrentTimeMs))
                {
                    UpdatePlayheadPosition();
                }
                else if (e.PropertyName == nameof(TimelineModel.TotalDurationMs) ||
                         e.PropertyName == nameof(TimelineModel.PixelsPerSecond))
                {
                    RedrawTimeline();
                }
            };

            foreach (var track in _viewModel.Timeline.Tracks)
            {
                track.Clips.CollectionChanged += OnClipsCollectionChanged;
            }

            _viewModel.PropertyChanged += (s, e) =>
            {
                if (e.PropertyName == nameof(MainViewModel.SelectedClip))
                {
                    UpdateClipSelectionHighlights();
                }
            };

            RedrawTimeline();
        }

        private void OnClipsCollectionChanged(object? sender, NotifyCollectionChangedEventArgs e)
        {
            RedrawClips();
        }

        public void RedrawTimeline()
        {
            if (_viewModel == null) return;

            double pps = _viewModel.Timeline.PixelsPerSecond;
            double totalSecs = _viewModel.Timeline.TotalDurationMs / 1000.0;
            double viewportWidth = Math.Max(TracksScrollViewer.ActualWidth, RulerScrollViewer.ActualWidth);
            if (viewportWidth <= 0)
            {
                viewportWidth = Math.Max(200, ActualWidth - 180);
            }
            double totalWidth = Math.Max(viewportWidth, totalSecs * pps + 200);

            RulerGrid.Width = totalWidth;
            RulerCanvas.Width = totalWidth;
            TracksGrid.Width = totalWidth;
            VideoTrackCanvas.Width = totalWidth;
            AudioTrackCanvas.Width = totalWidth;
            TextTrackCanvas.Width = totalWidth;
            ShapeTrackCanvas.Width = totalWidth;

            PlayheadLine.Height = TracksGrid.ActualHeight > 0 ? TracksGrid.ActualHeight : 210;

            RedrawRuler(totalWidth, pps);
            RedrawClips();
            UpdatePlayheadPosition();
        }

        private void RedrawRuler(double width, double pps)
        {
            RulerCanvas.Children.Clear();

            int stepSeconds = 1;
            if (pps < 20) stepSeconds = 10;
            else if (pps < 40) stepSeconds = 5;
            else if (pps < 80) stepSeconds = 2;

            double maxSecs = width / pps;

            for (int sec = 0; sec <= maxSecs; sec += stepSeconds)
            {
                double x = sec * pps;

                // Major tick line
                var tick = new Line
                {
                    X1 = x,
                    Y1 = 20,
                    X2 = x,
                    Y2 = 34,
                    Stroke = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 90, 90, 105)),
                    StrokeThickness = 1
                };
                RulerCanvas.Children.Add(tick);

                // Time label
                var label = new TextBlock
                {
                    Text = TimelineModel.FormatTimeCode(sec * 1000),
                    FontSize = 10,
                    Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 140, 140, 155)),
                    Margin = new Thickness(x + 4, 4, 0, 0)
                };
                RulerCanvas.Children.Add(label);
            }
        }

        private void RedrawClips()
        {
            if (_viewModel == null) return;

            VideoTrackCanvas.Children.Clear();
            AudioTrackCanvas.Children.Clear();
            TextTrackCanvas.Children.Clear();
            ShapeTrackCanvas.Children.Clear();

            double pps = _viewModel.Timeline.PixelsPerSecond;

            foreach (var track in _viewModel.Timeline.Tracks)
            {
                Canvas targetCanvas = track.TrackType switch
                {
                    TrackType.Video => VideoTrackCanvas,
                    TrackType.Audio => AudioTrackCanvas,
                    TrackType.Text => TextTrackCanvas,
                    TrackType.Shape => ShapeTrackCanvas,
                    _ => VideoTrackCanvas
                };

                foreach (var clip in track.Clips)
                {
                    var clipElement = CreateClipVisual(clip, pps);
                    targetCanvas.Children.Add(clipElement);
                }
            }
        }

        private FrameworkElement CreateClipVisual(ClipModel clip, double pps)
        {
            double left = (clip.StartTimeMs / 1000.0) * pps;
            double width = Math.Max(20, (clip.DurationMs / 1000.0) * pps);

            var border = new Border
            {
                Width = width,
                Height = 40,
                CornerRadius = new CornerRadius(4),
                BorderThickness = new Thickness(clip.IsSelected ? 2 : 1),
                BorderBrush = new SolidColorBrush(clip.IsSelected ?
                    Windows.UI.Color.FromArgb(255, 255, 255, 255) :
                    Windows.UI.Color.FromArgb(100, 255, 255, 255)),
                Tag = clip
            };

            // Distinct gradient per track type
            if (clip.TrackType == TrackType.Video)
            {
                border.Background = new LinearGradientBrush
                {
                    StartPoint = new Point(0, 0),
                    EndPoint = new Point(1, 1),
                    GradientStops =
                    {
                        new GradientStop { Color = Windows.UI.Color.FromArgb(255, 16, 78, 139), Offset = 0 },
                        new GradientStop { Color = Windows.UI.Color.FromArgb(255, 24, 116, 205), Offset = 1 }
                    }
                };
            }
            else if (clip.TrackType == TrackType.Audio)
            {
                border.Background = new LinearGradientBrush
                {
                    StartPoint = new Point(0, 0),
                    EndPoint = new Point(1, 1),
                    GradientStops =
                    {
                        new GradientStop { Color = Windows.UI.Color.FromArgb(255, 14, 52, 42), Offset = 0 },
                        new GradientStop { Color = Windows.UI.Color.FromArgb(255, 22, 82, 66), Offset = 1 }
                    }
                };
            }
            else if (clip.TrackType == TrackType.Text)
            {
                border.Background = new LinearGradientBrush
                {
                    StartPoint = new Point(0, 0),
                    EndPoint = new Point(1, 1),
                    GradientStops =
                    {
                        new GradientStop { Color = Windows.UI.Color.FromArgb(255, 102, 51, 153), Offset = 0 },
                        new GradientStop { Color = Windows.UI.Color.FromArgb(255, 147, 112, 219), Offset = 1 }
                    }
                };
            }
            else
            {
                border.Background = new LinearGradientBrush
                {
                    StartPoint = new Point(0, 0),
                    EndPoint = new Point(1, 1),
                    GradientStops =
                    {
                        new GradientStop { Color = Windows.UI.Color.FromArgb(255, 180, 80, 20), Offset = 0 },
                        new GradientStop { Color = Windows.UI.Color.FromArgb(255, 220, 120, 40), Offset = 1 }
                    }
                };
            }

            var grid = new Grid { Padding = new Thickness(0, 0, 0, 0) };

            if (clip.TrackType == TrackType.Audio)
            {
                var waveformCanvas = new Canvas
                {
                    Width = width,
                    Height = 40,
                    IsHitTestVisible = false
                };

                if (clip.WaveformData != null && clip.WaveformData.Length > 0)
                {
                    int barSpacing = 3;
                    int numBars = Math.Max(1, (int)(width / barSpacing));
                    for (int i = 0; i < numBars; ++i)
                    {
                        int sampleIdx = (int)((double)i / numBars * clip.WaveformData.Length);
                        if (sampleIdx >= clip.WaveformData.Length) sampleIdx = clip.WaveformData.Length - 1;
                        float amp = clip.WaveformData[sampleIdx];
                        double barH = Math.Max(2.0, amp * 30.0);
                        double barY = 20.0 - barH / 2.0;

                        var bar = new Rectangle
                        {
                            Width = 2,
                            Height = barH,
                            RadiusX = 1,
                            RadiusY = 1,
                            Fill = new SolidColorBrush(Windows.UI.Color.FromArgb(190, 0, 230, 118)),
                            IsHitTestVisible = false
                        };
                        Canvas.SetLeft(bar, i * barSpacing + 1);
                        Canvas.SetTop(bar, barY);
                        waveformCanvas.Children.Add(bar);
                    }
                }
                else
                {
                    var centerLine = new Line
                    {
                        X1 = 0,
                        Y1 = 20,
                        X2 = width,
                        Y2 = 20,
                        Stroke = new SolidColorBrush(Windows.UI.Color.FromArgb(120, 0, 230, 118)),
                        StrokeThickness = 1.5,
                        IsHitTestVisible = false
                    };
                    waveformCanvas.Children.Add(centerLine);
                }

                grid.Children.Add(waveformCanvas);
            }

            var tb = new TextBlock
            {
                Text = clip.Title,
                FontSize = 11,
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 255, 255, 255)),
                VerticalAlignment = clip.TrackType == TrackType.Audio ? VerticalAlignment.Top : VerticalAlignment.Center,
                Margin = new Thickness(8, clip.TrackType == TrackType.Audio ? 3 : 0, 8, 0),
                TextTrimming = TextTrimming.CharacterEllipsis
            };
            grid.Children.Add(tb);
            border.Child = grid;

            Canvas.SetLeft(border, left);
            Canvas.SetTop(border, 4);

            if (clip.TrackType == TrackType.Video)
            {
                // Main video track clip is pinned to StartTimeMs = 0!
                // Clicking selects the video clip and seeks to the clicked track position
                border.PointerPressed += (s, e) =>
                {
                    if (_viewModel != null)
                    {
                        _viewModel.SelectedClip = clip;
                        Point pt = e.GetCurrentPoint(TracksGrid).Position;
                        SeekFromRulerPosition(pt.X);
                    }
                    e.Handled = true;
                };
            }
            else
            {
                // 1. Right grip handle for extending / trimming clip duration
                var rightGrip = new Border
                {
                    Width = 10,
                    HorizontalAlignment = HorizontalAlignment.Right,
                    VerticalAlignment = VerticalAlignment.Stretch,
                    Background = new SolidColorBrush(Windows.UI.Color.FromArgb(90, 255, 255, 255)),
                    CornerRadius = new CornerRadius(0, 3, 3, 0)
                };
                ToolTipService.SetToolTip(rightGrip, "拖动右侧延长或缩短显示时间");
                grid.Children.Add(rightGrip);

                bool isResizingDuration = false;
                Point resizeStartPt = new Point();
                long initialDuration = clip.DurationMs;

                rightGrip.PointerPressed += (s, e) =>
                {
                    isResizingDuration = true;
                    resizeStartPt = e.GetCurrentPoint(TracksGrid).Position;
                    initialDuration = clip.DurationMs;
                    rightGrip.CapturePointer(e.Pointer);

                    if (_viewModel != null)
                    {
                        _viewModel.SelectedClip = clip;
                    }
                    e.Handled = true;
                };

                rightGrip.PointerMoved += (s, e) =>
                {
                    if (isResizingDuration && _viewModel != null)
                    {
                        Point curPt = e.GetCurrentPoint(TracksGrid).Position;
                        double dx = curPt.X - resizeStartPt.X;
                        long deltaMs = (long)((dx / _viewModel.Timeline.PixelsPerSecond) * 1000);
                        long newDuration = Math.Max(200, initialDuration + deltaMs);

                        clip.DurationMs = newDuration;
                        border.Width = Math.Max(20, (newDuration / 1000.0) * _viewModel.Timeline.PixelsPerSecond);
                        _viewModel.SyncClipToEngine(clip);
                        if (_viewModel.EngineHandle != IntPtr.Zero)
                        {
                            NativeEngineApi.Engine_RenderFrame(_viewModel.EngineHandle);
                        }
                        e.Handled = true;
                    }
                };

                rightGrip.PointerReleased += (s, e) =>
                {
                    if (isResizingDuration)
                    {
                        isResizingDuration = false;
                        rightGrip.ReleasePointerCapture(e.Pointer);
                        if (clip.TrackType == TrackType.Audio)
                        {
                            RedrawClips();
                        }
                        e.Handled = true;
                    }
                };

                // 2. Dragging clip horizontally for Text and Shape overlays
                bool isDragging = false;
                Point startPt = new Point();
                long initialStart = clip.StartTimeMs;

                border.PointerPressed += (s, e) =>
                {
                    isDragging = true;
                    startPt = e.GetCurrentPoint(TracksGrid).Position;
                    initialStart = clip.StartTimeMs;
                    border.CapturePointer(e.Pointer);

                    // Select and Seek
                    if (_viewModel != null)
                    {
                        _viewModel.SelectedClip = clip;
                        _viewModel.SeekTo(clip.StartTimeMs);
                    }
                    e.Handled = true;
                };

                border.PointerMoved += (s, e) =>
                {
                    if (isDragging && _viewModel != null)
                    {
                        Point curPt = e.GetCurrentPoint(TracksGrid).Position;
                        double dx = curPt.X - startPt.X;
                        if (Math.Abs(dx) > 3)
                        {
                            long deltaMs = (long)((dx / _viewModel.Timeline.PixelsPerSecond) * 1000);
                            long newStart = Math.Max(0, initialStart + deltaMs);

                            clip.StartTimeMs = newStart;
                            Canvas.SetLeft(border, (newStart / 1000.0) * _viewModel.Timeline.PixelsPerSecond);
                            _viewModel.SyncClipToEngine(clip);
                            _viewModel.SeekTo(newStart);
                        }
                        e.Handled = true;
                    }
                };

                border.PointerReleased += (s, e) =>
                {
                    if (isDragging)
                    {
                        isDragging = false;
                        border.ReleasePointerCapture(e.Pointer);
                        e.Handled = true;
                    }
                };
            }

            return border;
        }

        private void OnTracksGridPointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (_viewModel != null)
            {
                Point pt = e.GetCurrentPoint(TracksGrid).Position;
                SeekFromRulerPosition(pt.X);
            }
        }

        private void UpdateClipSelectionHighlights()
        {
            void HighlightCanvas(Canvas canvas)
            {
                foreach (var child in canvas.Children.OfType<Border>())
                {
                    if (child.Tag is ClipModel clip)
                    {
                        bool isSel = (_viewModel?.SelectedClip == clip);
                        child.BorderThickness = new Thickness(isSel ? 2 : 1);
                        child.BorderBrush = new SolidColorBrush(isSel ?
                            Windows.UI.Color.FromArgb(255, 255, 255, 255) :
                            Windows.UI.Color.FromArgb(100, 255, 255, 255));
                    }
                }
            }

            HighlightCanvas(VideoTrackCanvas);
            HighlightCanvas(AudioTrackCanvas);
            HighlightCanvas(TextTrackCanvas);
            HighlightCanvas(ShapeTrackCanvas);
        }

        private void UpdatePlayheadPosition()
        {
            if (_viewModel == null) return;

            double pps = _viewModel.Timeline.PixelsPerSecond;
            double x = (_viewModel.Timeline.CurrentTimeMs / 1000.0) * pps;

            Canvas.SetLeft(PlayheadLine, x);
            Canvas.SetLeft(RulerPlayheadHead, x);
        }

        private void OnRulerPointerPressed(object sender, PointerRoutedEventArgs e)
        {
            _isDraggingRuler = true;
            RulerCanvas.CapturePointer(e.Pointer);
            SeekFromRulerPosition(e.GetCurrentPoint(RulerCanvas).Position.X);
            e.Handled = true;
        }

        private void OnRulerPointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (_isDraggingRuler)
            {
                SeekFromRulerPosition(e.GetCurrentPoint(RulerCanvas).Position.X);
                e.Handled = true;
            }
        }

        private void OnRulerPointerReleased(object sender, PointerRoutedEventArgs e)
        {
            if (_isDraggingRuler)
            {
                _isDraggingRuler = false;
                RulerCanvas.ReleasePointerCapture(e.Pointer);
                e.Handled = true;
            }
        }

        private void SeekFromRulerPosition(double x)
        {
            if (_viewModel == null) return;
            double pps = _viewModel.Timeline.PixelsPerSecond;
            long timeMs = (long)Math.Max(0, (x / pps) * 1000.0);
            _viewModel.SeekTo(timeMs);
        }

        private bool _isSyncingScroll = false;
        private void OnTracksScrollViewChanged(object sender, ScrollViewerViewChangedEventArgs e)
        {
            if (_isSyncingScroll) return;
            _isSyncingScroll = true;
            RulerScrollViewer.ChangeView(TracksScrollViewer.HorizontalOffset, null, null, true);
            _isSyncingScroll = false;
        }

        private void OnRulerScrollViewChanged(object sender, ScrollViewerViewChangedEventArgs e)
        {
            if (_isSyncingScroll) return;
            _isSyncingScroll = true;
            TracksScrollViewer.ChangeView(RulerScrollViewer.HorizontalOffset, null, null, true);
            _isSyncingScroll = false;
        }

        private void OnZoomInClicked(object sender, RoutedEventArgs e)
        {
            if (_viewModel != null)
            {
                _viewModel.Timeline.PixelsPerSecond += 15.0;
            }
        }

        private void OnZoomOutClicked(object sender, RoutedEventArgs e)
        {
            if (_viewModel != null)
            {
                _viewModel.Timeline.PixelsPerSecond -= 15.0;
            }
        }
    }
}
