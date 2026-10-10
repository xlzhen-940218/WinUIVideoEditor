using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace WinUIVideoEditor.Models
{
    public class TimelineModel : INotifyPropertyChanged
    {
        private long _currentTimeMs;
        private long _totalDurationMs = 30000; // default 30 seconds
        private double _pixelsPerSecond = 50.0; // scale factor

        public event PropertyChangedEventHandler? PropertyChanged;

        private void OnPropertyChanged([CallerMemberName] string? name = null)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }

        public ObservableCollection<TrackModel> Tracks { get; set; } = new();

        public long CurrentTimeMs
        {
            get => _currentTimeMs;
            set
            {
                if (_currentTimeMs != value)
                {
                    _currentTimeMs = Math.Clamp(value, 0, _totalDurationMs);
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
                if (_totalDurationMs != value)
                {
                    _totalDurationMs = Math.Max(1000, value);
                    OnPropertyChanged();
                    OnPropertyChanged(nameof(TotalDurationFormatted));
                    OnPropertyChanged(nameof(TimelineWidth));
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
