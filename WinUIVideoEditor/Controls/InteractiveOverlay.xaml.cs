using System;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.Foundation;
using WinUIVideoEditor.Models;
using WinUIVideoEditor.ViewModels;

namespace WinUIVideoEditor.Controls
{
    public sealed partial class InteractiveOverlay : UserControl
    {
        private MainViewModel? _viewModel;
        private bool _isDraggingBox = false;
        private Point _dragStartPoint;
        private float _initialX;
        private float _initialY;

        private bool _isDraggingHandle = false;
        private string _activeHandle = "";
        private float _initialWidth;
        private float _initialHeight;

        public InteractiveOverlay()
        {
            this.InitializeComponent();
        }

        public void BindViewModel(MainViewModel viewModel)
        {
            _viewModel = viewModel;
            _viewModel.PropertyChanged += (s, e) =>
            {
                if (e.PropertyName == nameof(MainViewModel.SelectedClip))
                {
                    UpdateBoundingBox();
                }
            };
            UpdateBoundingBox();
        }

        public void Refresh()
        {
            UpdateBoundingBox();
        }

        private void UpdateBoundingBox()
        {
            var clip = _viewModel?.SelectedClip;
            if (clip == null || clip.TrackType == TrackType.Video || clip.TrackType == TrackType.Audio)
            {
                BoundingBoxContainer.Visibility = Visibility.Collapsed;
                return;
            }

            BoundingBoxContainer.Visibility = Visibility.Visible;

            double x = clip.X;
            double y = clip.Y;
            double w = Math.Max(20, clip.Width);
            double h = Math.Max(20, clip.Height);

            Canvas.SetLeft(SelectionBox, x);
            Canvas.SetTop(SelectionBox, y);
            SelectionBox.Width = w;
            SelectionBox.Height = h;

            double handleSize = 10;
            double halfH = handleSize / 2;

            // NW, NE, SW, SE
            Canvas.SetLeft(HandleNW, x - halfH);
            Canvas.SetTop(HandleNW, y - halfH);

            Canvas.SetLeft(HandleNE, x + w - halfH);
            Canvas.SetTop(HandleNE, y - halfH);

            Canvas.SetLeft(HandleSW, x - halfH);
            Canvas.SetTop(HandleSW, y + h - halfH);

            Canvas.SetLeft(HandleSE, x + w - halfH);
            Canvas.SetTop(HandleSE, y + h - halfH);

            // N, S, W, E
            Canvas.SetLeft(HandleN, x + w / 2 - halfH);
            Canvas.SetTop(HandleN, y - halfH);

            Canvas.SetLeft(HandleS, x + w / 2 - halfH);
            Canvas.SetTop(HandleS, y + h - halfH);

            Canvas.SetLeft(HandleW, x - halfH);
            Canvas.SetTop(HandleW, y + h / 2 - halfH);

            Canvas.SetLeft(HandleE, x + w - halfH);
            Canvas.SetTop(HandleE, y + h / 2 - halfH);
        }

        private void OnCanvasPointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (_viewModel == null) return;

            Point pt = e.GetCurrentPoint(OverlayCanvas).Position;

            // Check if clicking on any clip in the timeline
            foreach (var track in _viewModel.Timeline.Tracks)
            {
                if (track.TrackType == TrackType.Video || track.TrackType == TrackType.Audio) continue;

                foreach (var clip in track.Clips)
                {
                    if (pt.X >= clip.X && pt.X <= clip.X + clip.Width &&
                        pt.Y >= clip.Y && pt.Y <= clip.Y + clip.Height)
                    {
                        _viewModel.SelectedClip = clip;
                        _viewModel.SeekTo(clip.StartTimeMs);
                        return;
                    }
                }
            }
        }

        private void OnBoxPointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (_viewModel?.SelectedClip == null) return;

            _isDraggingBox = true;
            _dragStartPoint = e.GetCurrentPoint(OverlayCanvas).Position;
            _initialX = _viewModel.SelectedClip.X;
            _initialY = _viewModel.SelectedClip.Y;
            SelectionBox.CapturePointer(e.Pointer);
            e.Handled = true;
        }

        private void OnBoxPointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (!_isDraggingBox || _viewModel?.SelectedClip == null) return;

            Point currentPoint = e.GetCurrentPoint(OverlayCanvas).Position;
            double dx = currentPoint.X - _dragStartPoint.X;
            double dy = currentPoint.Y - _dragStartPoint.Y;

            _viewModel.SelectedClip.X = (float)Math.Max(0, _initialX + dx);
            _viewModel.SelectedClip.Y = (float)Math.Max(0, _initialY + dy);

            UpdateBoundingBox();
            _viewModel.SyncClipToEngine(_viewModel.SelectedClip);
            e.Handled = true;
        }

        private void OnBoxPointerReleased(object sender, PointerRoutedEventArgs e)
        {
            if (_isDraggingBox)
            {
                _isDraggingBox = false;
                SelectionBox.ReleasePointerCapture(e.Pointer);
                e.Handled = true;
            }
        }

        private void OnHandlePointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (_viewModel?.SelectedClip == null || sender is not FrameworkElement fe) return;

            _isDraggingHandle = true;
            _activeHandle = fe.Tag?.ToString() ?? "";
            _dragStartPoint = e.GetCurrentPoint(OverlayCanvas).Position;
            _initialX = _viewModel.SelectedClip.X;
            _initialY = _viewModel.SelectedClip.Y;
            _initialWidth = _viewModel.SelectedClip.Width;
            _initialHeight = _viewModel.SelectedClip.Height;

            fe.CapturePointer(e.Pointer);
            e.Handled = true;
        }

        private void OnHandlePointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (!_isDraggingHandle || _viewModel?.SelectedClip == null) return;

            Point currentPoint = e.GetCurrentPoint(OverlayCanvas).Position;
            double dx = currentPoint.X - _dragStartPoint.X;
            double dy = currentPoint.Y - _dragStartPoint.Y;

            var clip = _viewModel.SelectedClip;

            switch (_activeHandle)
            {
                case "E":
                    clip.Width = (float)Math.Max(20, _initialWidth + dx);
                    break;
                case "S":
                    clip.Height = (float)Math.Max(20, _initialHeight + dy);
                    break;
                case "SE":
                    clip.Width = (float)Math.Max(20, _initialWidth + dx);
                    clip.Height = (float)Math.Max(20, _initialHeight + dy);
                    break;
                case "W":
                    float newW = (float)Math.Max(20, _initialWidth - dx);
                    clip.X = _initialX + (_initialWidth - newW);
                    clip.Width = newW;
                    break;
                case "N":
                    float newH = (float)Math.Max(20, _initialHeight - dy);
                    clip.Y = _initialY + (_initialHeight - newH);
                    clip.Height = newH;
                    break;
                case "NW":
                    float nwW = (float)Math.Max(20, _initialWidth - dx);
                    float nwH = (float)Math.Max(20, _initialHeight - dy);
                    clip.X = _initialX + (_initialWidth - nwW);
                    clip.Y = _initialY + (_initialHeight - nwH);
                    clip.Width = nwW;
                    clip.Height = nwH;
                    break;
                case "NE":
                    float neH = (float)Math.Max(20, _initialHeight - dy);
                    clip.Y = _initialY + (_initialHeight - neH);
                    clip.Width = (float)Math.Max(20, _initialWidth + dx);
                    clip.Height = neH;
                    break;
                case "SW":
                    float swW = (float)Math.Max(20, _initialWidth - dx);
                    clip.X = _initialX + (_initialWidth - swW);
                    clip.Width = swW;
                    clip.Height = (float)Math.Max(20, _initialHeight + dy);
                    break;
            }

            UpdateBoundingBox();
            _viewModel.SyncClipToEngine(clip);
            e.Handled = true;
        }

        private void OnHandlePointerReleased(object sender, PointerRoutedEventArgs e)
        {
            if (_isDraggingHandle && sender is FrameworkElement fe)
            {
                _isDraggingHandle = false;
                fe.ReleasePointerCapture(e.Pointer);
                e.Handled = true;
            }
        }
    }
}
