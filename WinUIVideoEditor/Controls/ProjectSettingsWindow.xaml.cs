using System;
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using WinRT.Interop;

namespace WinUIVideoEditor.Controls
{
    public sealed partial class ProjectSettingsWindow : Window
    {
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

        public ProjectSettingsWindow(IntPtr parentHwnd, string mediaPath, string durationFormatted)
        {
            this.InitializeComponent();

            var hwnd = WindowNative.GetWindowHandle(this);
            var windowId = Win32Interop.GetWindowIdFromWindow(hwnd);
            var appWindow = AppWindow.GetFromWindowId(windowId);
            appWindow.Title = "工程与引擎设置";

            int winW = 500;
            int winH = 390;
            appWindow.Resize(new Windows.Graphics.SizeInt32(winW, winH));

            if (parentHwnd != IntPtr.Zero)
            {
                SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, parentHwnd);
                if (GetWindowRect(parentHwnd, out RECT parentRect))
                {
                    int centerX = (parentRect.Left + parentRect.Right) / 2;
                    int centerY = (parentRect.Top + parentRect.Bottom) / 2;
                    int posX = Math.Max(50, centerX - (winW / 2));
                    int posY = Math.Max(50, centerY - (winH / 2));
                    appWindow.Move(new Windows.Graphics.PointInt32(posX, posY));
                }
            }

            TxtMediaPath.Text = string.IsNullOrEmpty(mediaPath) ? "未加载" : mediaPath;
            TxtDuration.Text = durationFormatted;
        }

        private void OnCloseClicked(object sender, RoutedEventArgs e)
        {
            this.Close();
        }
    }
}
