using System;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using Windows.UI;
using WinUIVideoEditor.Native;

namespace WinUIVideoEditor.Models
{
    public enum TrackType
    {
        Video = 0,
        Text = 1,
        Shape = 2
    }

    public class ClipModel : INotifyPropertyChanged
    {
        private int _id;
        private string _title = "新建图元";
        private TrackType _trackType;
        private int _elementType; // 0: Text, 1: Rectangle, 2: Circle
        private long _startTimeMs;
        private long _durationMs = 3000;
        private float _x = 100;
        private float _y = 100;
        private float _width = 300;
        private float _height = 100;
        private Color _fillColor = Color.FromArgb(255, 255, 255, 255);
        private Color _strokeColor = Color.FromArgb(255, 0, 0, 0);
        private float _strokeWidth = 2.0f;
        private float _fontSize = 36.0f;
        private string _fontFamily = "Segoe UI";
        private string _contentText = "双击输入文本";
        private bool _isSelected;

        public event PropertyChangedEventHandler? PropertyChanged;

        private void OnPropertyChanged([CallerMemberName] string? name = null)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }

        public int Id
        {
            get => _id;
            set { if (_id != value) { _id = value; OnPropertyChanged(); } }
        }

        public string Title
        {
            get => _title;
            set { if (_title != value) { _title = value; OnPropertyChanged(); } }
        }

        public TrackType TrackType
        {
            get => _trackType;
            set { if (_trackType != value) { _trackType = value; OnPropertyChanged(); } }
        }

        public int ElementType
        {
            get => _elementType;
            set { if (_elementType != value) { _elementType = value; OnPropertyChanged(); } }
        }

        public long StartTimeMs
        {
            get => _startTimeMs;
            set { if (_startTimeMs != value) { _startTimeMs = value; OnPropertyChanged(); OnPropertyChanged(nameof(EndTimeMs)); } }
        }

        public long DurationMs
        {
            get => _durationMs;
            set { if (_durationMs != value) { _durationMs = value; OnPropertyChanged(); OnPropertyChanged(nameof(EndTimeMs)); } }
        }

        public long EndTimeMs => _startTimeMs + _durationMs;

        public float X
        {
            get => _x;
            set { if (Math.Abs(_x - value) > 0.01f) { _x = value; OnPropertyChanged(); } }
        }

        public float Y
        {
            get => _y;
            set { if (Math.Abs(_y - value) > 0.01f) { _y = value; OnPropertyChanged(); } }
        }

        public float Width
        {
            get => _width;
            set { if (Math.Abs(_width - value) > 0.01f) { _width = value; OnPropertyChanged(); } }
        }

        public float Height
        {
            get => _height;
            set { if (Math.Abs(_height - value) > 0.01f) { _height = value; OnPropertyChanged(); } }
        }

        public Color FillColor
        {
            get => _fillColor;
            set { if (_fillColor != value) { _fillColor = value; OnPropertyChanged(); OnPropertyChanged(nameof(FillColorUint)); } }
        }

        public Color StrokeColor
        {
            get => _strokeColor;
            set { if (_strokeColor != value) { _strokeColor = value; OnPropertyChanged(); OnPropertyChanged(nameof(StrokeColorUint)); } }
        }

        public uint FillColorUint => ((uint)_fillColor.A << 24) | ((uint)_fillColor.R << 16) | ((uint)_fillColor.G << 8) | _fillColor.B;
        public uint StrokeColorUint => ((uint)_strokeColor.A << 24) | ((uint)_strokeColor.R << 16) | ((uint)_strokeColor.G << 8) | _strokeColor.B;

        public float StrokeWidth
        {
            get => _strokeWidth;
            set { if (Math.Abs(_strokeWidth - value) > 0.01f) { _strokeWidth = value; OnPropertyChanged(); } }
        }

        public float FontSize
        {
            get => _fontSize;
            set { if (Math.Abs(_fontSize - value) > 0.01f) { _fontSize = value; OnPropertyChanged(); } }
        }

        public string FontFamily
        {
            get => _fontFamily;
            set { if (_fontFamily != value) { _fontFamily = value; OnPropertyChanged(); } }
        }

        public string ContentText
        {
            get => _contentText;
            set { if (_contentText != value) { _contentText = value; OnPropertyChanged(); } }
        }

        public bool IsSelected
        {
            get => _isSelected;
            set { if (_isSelected != value) { _isSelected = value; OnPropertyChanged(); } }
        }

        public NativeEngineApi.PrimitiveElementDesc ToNativeDesc()
        {
            return new NativeEngineApi.PrimitiveElementDesc
            {
                ElementId = _id,
                ElementType = _elementType,
                X = _x,
                Y = _y,
                Width = _width,
                Height = _height,
                FillColor = FillColorUint,
                StrokeColor = StrokeColorUint,
                StrokeWidth = _strokeWidth,
                FontSize = _fontSize,
                FontFamily = _fontFamily,
                ContentText = _contentText,
                StartTimeMs = _startTimeMs,
                DurationMs = _durationMs,
                IsSelected = _isSelected ? 1 : 0
            };
        }
    }
}
