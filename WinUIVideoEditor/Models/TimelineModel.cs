using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace WinUIVideoEditor.Models
{
    public class TimelineModel : INotifyPropertyChanged
    {
        private long _currentTimeMs;
        private long _totalDurationMs = 0; // default 0 before opening media
        private double _pixelsPerSecond = 50.0; // scale factor

        public event PropertyChangedEventHandler? PropertyChanged;

        private void OnPropertyChanged([CallerMemberName] string? name = null)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }

        public ObservableCollection<TrackModel> Tracks { get; set; } = new();

        public bool CanPlay => _totalDurationMs > 0;

        public long CurrentTimeMs
        {
            get => _currentTimeMs;
            set
            {
                long clamped = _totalDurationMs > 0 ? Math.Clamp(value, 0, _totalDurationMs) : 0;
                if (_currentTimeMs != clamped)
                {
                    _currentTimeMs = clamped;
                    OnPropertyChanged();
                    OnPropertyChanged(nameof(CurrentTimeFormatted));
                    OnPropertyChanged(nameof(PlayheadPositionX));
                }
            }
        }

        public long TotalDurationMs
        {
            get => _totalDurationMs;
            set
            {
                long clamped = Math.Max(0, value);
                if (_totalDurationMs != clamped)
                {
                    _totalDurationMs = clamped;
                    OnPropertyChanged();
                    OnPropertyChanged(nameof(TotalDurationFormatted));
                    OnPropertyChanged(nameof(TimelineWidth));
                    OnPropertyChanged(nameof(CanPlay));
                    if (_currentTimeMs > _totalDurationMs)
                    {
                        CurrentTimeMs = _totalDurationMs;
                    }
                }
            }
        }

        public double PixelsPerSecond
        {
            get => _pixelsPerSecond;
            set
            {
                if (Math.Abs(_pixelsPerSecond - value) > 0.01)
                {
                    _pixelsPerSecond = Math.Clamp(value, 10.0, 300.0);
                    OnPropertyChanged();
                    OnPropertyChanged(nameof(TimelineWidth));
                    OnPropertyChanged(nameof(PlayheadPositionX));
                }
            }
        }

        public double TimelineWidth => (_totalDurationMs / 1000.0) * _pixelsPerSecond;

        public double PlayheadPositionX => (_currentTimeMs / 1000.0) * _pixelsPerSecond;

        public string CurrentTimeFormatted => FormatTimeCode(_currentTimeMs);
        public string TotalDurationFormatted => FormatTimeCode(_totalDurationMs);

        public static string FormatTimeCode(long timeMs)
        {
            TimeSpan ts = TimeSpan.FromMilliseconds(timeMs);
            int frames = (int)((timeMs % 1000) * 30 / 1000); // 30 fps
            return $"{ts.Hours:D2}:{ts.Minutes:D2}:{ts.Seconds:D2}:{frames:D2}";
        }
    }
}
