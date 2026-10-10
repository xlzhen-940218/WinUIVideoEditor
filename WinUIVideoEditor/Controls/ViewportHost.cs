using System;
using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using WinUIVideoEditor.Native;

namespace WinUIVideoEditor.Controls
{
    public class ViewportHost : Grid
    {
        private const int WS_CHILD = 0x40000000;
        private const int WS_VISIBLE = 0x10000000;
        private const int WS_CLIPCHILDREN = 0x02000000;
        private const int WS_CLIPSIBLINGS = 0x04000000;

        private const uint SWP_NOZORDER = 0x0004;
        private const uint SWP_NOACTIVATE = 0x0010;
        private const uint SWP_SHOWWINDOW = 0x0040;

        [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern ushort RegisterClassEx(ref WNDCLASSEX lpwcx);

        [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern IntPtr CreateWindowEx(
            uint dwExStyle,
            string lpClassName,
            string lpWindowName,
            uint dwStyle,
            int x,
            int y,
            int nWidth,
            int nHeight,
            IntPtr hWndParent,
            IntPtr hMenu,
            IntPtr hInstance,
            IntPtr lpParam);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool DestroyWindow(IntPtr hWnd);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool SetWindowPos(
            IntPtr hWnd,
            IntPtr hWndInsertAfter,
            int X,
            int Y,
            int cx,
            int cy,
            uint uFlags);

        [DllImport("user32.dll")]
        private static extern IntPtr DefWindowProc(IntPtr hWnd, uint uMsg, IntPtr wParam, IntPtr lParam);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr GetModuleHandle(string? lpModuleName);

        private delegate IntPtr WndProcDelegate(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct WNDCLASSEX
        {
            public uint cbSize;
            public uint style;
            public WndProcDelegate lpfnWndProc;
            public int cbClsExtra;
            public int cbWndExtra;
            public IntPtr hInstance;
            public IntPtr hIcon;
            public IntPtr hCursor;
            public IntPtr hbrBackground;
            public string lpszMenuName;
            public string lpszClassName;
            public IntPtr hIconSm;
        }

        private static bool _classRegistered = false;
        private static WndProcDelegate? _wndProc;
        private const string WindowClassName = "WinUIVideoEditorViewportChild";

        [DllImport("user32.dll")]
        private static extern uint GetDpiForWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern IntPtr SetCapture(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern bool ReleaseCapture();

        private const uint WM_MOUSEMOVE = 0x0200;
        private const uint WM_LBUTTONDOWN = 0x0201;
        private const uint WM_LBUTTONUP = 0x0202;

        private static readonly System.Collections.Generic.Dictionary<IntPtr, ViewportHost> _hosts = new();

        private IntPtr _parentHwnd = IntPtr.Zero;
        private IntPtr _childHwnd = IntPtr.Zero;
        private IntPtr _engine = IntPtr.Zero;
        private ViewModels.MainViewModel? _viewModel;

        private bool _isDraggingBox = false;
        private bool _isDraggingHandle = false;
        private string _activeHandle = "";
        private Windows.Foundation.Point _dragStartDip;
        private float _initialX;
        private float _initialY;
        private float _initialWidth;
        private float _initialHeight;

        public IntPtr EngineHandle => _engine;

        public event Action<IntPtr>? EngineInitialized;

        public ViewportHost()
        {
            Loaded += OnLoaded;
            Unloaded += OnUnloaded;
            SizeChanged += OnSizeChanged;
        }

        public void BindViewModel(ViewModels.MainViewModel viewModel)
        {
            _viewModel = viewModel;
        }

        public double GetDpiScale()
        {
            if (_parentHwnd != IntPtr.Zero)
            {
                uint dpi = GetDpiForWindow(_parentHwnd);
                if (dpi > 0) return dpi / 96.0;
            }
            if (this.XamlRoot != null && this.XamlRoot.RasterizationScale > 0)
            {
                return this.XamlRoot.RasterizationScale;
            }
            return 1.0;
        }

        public void SetParentHwnd(IntPtr parentHwnd)
        {
            _parentHwnd = parentHwnd;
            if (ActualWidth > 0 && ActualHeight > 0)
            {
                InitializeChildWindow();
            }
        }

        private void OnLoaded(object sender, RoutedEventArgs e)
        {
            if (_childHwnd == IntPtr.Zero && _parentHwnd != IntPtr.Zero && ActualWidth > 0 && ActualHeight > 0)
            {
                InitializeChildWindow();
            }
            else
            {
                UpdateChildWindowPosition();
            }
        }

        private void InitializeChildWindow()
        {
            if (_childHwnd != IntPtr.Zero || _parentHwnd == IntPtr.Zero) return;
            if (ActualWidth <= 0 || ActualHeight <= 0) return;

            double scale = GetDpiScale();
            int physicalW = (int)Math.Max(100, Math.Round(ActualWidth * scale));
            int physicalH = (int)Math.Max(100, Math.Round(ActualHeight * scale));

            RegisterWindowClass();

            IntPtr hInstance = GetModuleHandle(null);
            _childHwnd = CreateWindowEx(
                0,
                WindowClassName,
                "ViewportChild",
                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                0,
                0,
                physicalW,
                physicalH,
                _parentHwnd,
                IntPtr.Zero,
                hInstance,
                IntPtr.Zero);

            if (_childHwnd != IntPtr.Zero)
            {
                _hosts[_childHwnd] = this;
                _engine = NativeEngineApi.Engine_Initialize(_childHwnd, physicalW, physicalH);
                EngineInitialized?.Invoke(_engine);
                UpdateChildWindowPosition();
            }
        }

        private static IntPtr CustomWndProc(IntPtr hWnd, uint uMsg, IntPtr wParam, IntPtr lParam)
        {
            if (_hosts.TryGetValue(hWnd, out var host))
            {
                return host.HandleWindowMessage(hWnd, uMsg, wParam, lParam);
            }
            return DefWindowProc(hWnd, uMsg, wParam, lParam);
        }

        private IntPtr HandleWindowMessage(IntPtr hWnd, uint uMsg, IntPtr wParam, IntPtr lParam)
        {
            switch (uMsg)
            {
                case WM_LBUTTONDOWN:
                    {
                        short x = (short)(lParam.ToInt64() & 0xFFFF);
                        short y = (short)((lParam.ToInt64() >> 16) & 0xFFFF);
                        SetCapture(hWnd);
                        OnCanvasPointerDown(x, y);
                        return IntPtr.Zero;
                    }
                case WM_MOUSEMOVE:
                    {
                        short x = (short)(lParam.ToInt64() & 0xFFFF);
                        short y = (short)((lParam.ToInt64() >> 16) & 0xFFFF);
                        OnCanvasPointerMove(x, y);
                        return IntPtr.Zero;
                    }
                case WM_LBUTTONUP:
                    {
                        ReleaseCapture();
                        OnCanvasPointerUp();
                        return IntPtr.Zero;
                    }
            }
            return DefWindowProc(hWnd, uMsg, wParam, lParam);
        }

        private void OnCanvasPointerDown(short physX, short physY)
        {
            if (_viewModel == null) return;

            // Direct 1:1 coordinates matching Skia rendering canvas
            double canvasX = physX;
            double canvasY = physY;

            // 1. Check if clicked on a resize handle of the currently selected element
            var sel = _viewModel.SelectedClip;
            if (sel != null && sel.TrackType != Models.TrackType.Video)
            {
                string? hitHandle = HitTestHandle(sel, canvasX, canvasY);
                if (hitHandle != null)
                {
                    _isDraggingHandle = true;
                    _activeHandle = hitHandle;
                    _dragStartDip = new Windows.Foundation.Point(canvasX, canvasY);
                    _initialX = sel.X;
                    _initialY = sel.Y;
                    _initialWidth = sel.Width;
                    _initialHeight = sel.Height;
                    return;
                }

                // 2. Check if clicked inside the currently selected element body
                if (canvasX >= sel.X && canvasX <= sel.X + sel.Width &&
                    canvasY >= sel.Y && canvasY <= sel.Y + sel.Height)
                {
                    _isDraggingBox = true;
                    _dragStartDip = new Windows.Foundation.Point(canvasX, canvasY);
                    _initialX = sel.X;
                    _initialY = sel.Y;
                    return;
                }
            }

            // 3. Check if clicked on any other active element on canvas
            long curTime = _viewModel.Timeline.CurrentTimeMs;
            Models.ClipModel? hitClip = null;

            foreach (var track in System.Linq.Enumerable.Reverse(_viewModel.Timeline.Tracks))
            {
                if (track.TrackType == Models.TrackType.Video) continue;
                foreach (var clip in System.Linq.Enumerable.Reverse(track.Clips))
                {
                    if (curTime >= clip.StartTimeMs && curTime <= clip.EndTimeMs)
                    {
                        if (canvasX >= clip.X && canvasX <= clip.X + clip.Width &&
                            canvasY >= clip.Y && canvasY <= clip.Y + clip.Height)
                        {
                            hitClip = clip;
                            break;
                        }
                    }
                }
                if (hitClip != null) break;
            }

            if (hitClip != null)
            {
                _viewModel.SelectedClip = hitClip;
                _isDraggingBox = true;
                _dragStartDip = new Windows.Foundation.Point(canvasX, canvasY);
                _initialX = hitClip.X;
                _initialY = hitClip.Y;
            }
            else
            {
                _viewModel.SelectedClip = null;
                if (_engine != IntPtr.Zero)
                {
                    NativeEngineApi.Engine_RenderFrame(_engine);
                }
            }
        }

        private void OnCanvasPointerMove(short physX, short physY)
        {
            if (_viewModel?.SelectedClip == null) return;
            if (!_isDraggingBox && !_isDraggingHandle) return;

            double canvasX = physX;
            double canvasY = physY;

            var clip = _viewModel.SelectedClip;

            if (_isDraggingBox)
            {
                double dx = canvasX - _dragStartDip.X;
                double dy = canvasY - _dragStartDip.Y;
                clip.X = (float)Math.Max(0, _initialX + dx);
                clip.Y = (float)Math.Max(0, _initialY + dy);
                _viewModel.SyncClipToEngine(clip);
                if (_engine != IntPtr.Zero)
                {
                    NativeEngineApi.Engine_RenderFrame(_engine);
                }
            }
            else if (_isDraggingHandle)
            {
                double dx = canvasX - _dragStartDip.X;
                double dy = canvasY - _dragStartDip.Y;

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

                _viewModel.SyncClipToEngine(clip);
                if (_engine != IntPtr.Zero)
                {
                    NativeEngineApi.Engine_RenderFrame(_engine);
                }
            }
        }

        private void OnCanvasPointerUp()
        {
            _isDraggingBox = false;
            _isDraggingHandle = false;
        }

        private static string? HitTestHandle(Models.ClipModel clip, double x, double y)
        {
            double r = 10;
            bool Near(double hx, double hy) => Math.Abs(x - hx) <= r && Math.Abs(y - hy) <= r;

            if (Near(clip.X, clip.Y)) return "NW";
            if (Near(clip.X + clip.Width, clip.Y)) return "NE";
            if (Near(clip.X, clip.Y + clip.Height)) return "SW";
            if (Near(clip.X + clip.Width, clip.Y + clip.Height)) return "SE";
            if (Near(clip.X + clip.Width / 2, clip.Y)) return "N";
            if (Near(clip.X + clip.Width / 2, clip.Y + clip.Height)) return "S";
            if (Near(clip.X, clip.Y + clip.Height / 2)) return "W";
            if (Near(clip.X + clip.Width, clip.Y + clip.Height / 2)) return "E";
            return null;
        }

        private static void RegisterWindowClass()
        {
            if (_classRegistered) return;

            _wndProc = CustomWndProc;
            var wcx = new WNDCLASSEX
            {
                cbSize = (uint)Marshal.SizeOf<WNDCLASSEX>(),
                style = 0,
                lpfnWndProc = _wndProc,
                hInstance = GetModuleHandle(null),
                lpszClassName = WindowClassName
            };

            RegisterClassEx(ref wcx);
            _classRegistered = true;
        }

        private void OnSizeChanged(object sender, SizeChangedEventArgs e)
        {
            if (_childHwnd == IntPtr.Zero && _parentHwnd != IntPtr.Zero && ActualWidth > 0 && ActualHeight > 0)
            {
                InitializeChildWindow();
            }
            else
            {
                UpdateChildWindowPosition();
            }
        }

        public void UpdateChildWindowPosition()
        {
            if (_childHwnd == IntPtr.Zero || _parentHwnd == IntPtr.Zero) return;
            if (ActualWidth <= 0 || ActualHeight <= 0) return;

            try
            {
                double scale = GetDpiScale();
                GeneralTransform transform;
                if (this.XamlRoot?.Content != null)
                {
                    transform = TransformToVisual(this.XamlRoot.Content);
                }
                else
                {
                    transform = TransformToVisual(null);
                }

                Windows.Foundation.Point point = transform.TransformPoint(new Windows.Foundation.Point(0, 0));

                int physicalX = (int)Math.Round(point.X * scale);
                int physicalY = (int)Math.Round(point.Y * scale);
                int physicalW = (int)Math.Max(1, Math.Round(ActualWidth * scale));
                int physicalH = (int)Math.Max(1, Math.Round(ActualHeight * scale));

                SetWindowPos(
                    _childHwnd,
                    IntPtr.Zero,
                    physicalX,
                    physicalY,
                    physicalW,
                    physicalH,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);

                if (_engine != IntPtr.Zero)
                {
                    NativeEngineApi.Engine_Resize(_engine, physicalW, physicalH);
                }
            }
            catch
            {
                // Ignore transient layout exceptions during window minimize/restore
            }
        }

        private void OnUnloaded(object sender, RoutedEventArgs e)
        {
            if (_engine != IntPtr.Zero)
            {
                NativeEngineApi.Engine_Destroy(_engine);
                _engine = IntPtr.Zero;
            }

            if (_childHwnd != IntPtr.Zero)
            {
                _hosts.Remove(_childHwnd);
                DestroyWindow(_childHwnd);
                _childHwnd = IntPtr.Zero;
            }
        }
    }
}
