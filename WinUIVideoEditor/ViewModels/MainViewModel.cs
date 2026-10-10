using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Linq;
using System.Runtime.CompilerServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Windows.UI;
using WinUIVideoEditor.Models;
using WinUIVideoEditor.Native;

namespace WinUIVideoEditor.ViewModels
{
    public class MainViewModel : INotifyPropertyChanged
    {
        private IntPtr _engine = IntPtr.Zero;
        private TimelineModel _timeline = new();
        private ClipModel? _selectedClip;
        private bool _isPlaying;
        private string _mediaFilePath = "";
        private DispatcherTimer? _playbackTimer;
        private int _nextElementId = 1;

        public event PropertyChangedEventHandler? PropertyChanged;

        private void OnPropertyChanged([CallerMemberName] string? name = null)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }

        public TimelineModel Timeline => _timeline;
        public IntPtr EngineHandle => _engine;

        public ClipModel? SelectedClip
        {
            get => _selectedClip;
            set
            {
                if (_selectedClip != value)
                {
                    var prev = _selectedClip;
                    if (prev != null)
                    {
                        prev.IsSelected = false;
                        SyncClipToEngine(prev);
                    }
                    _selectedClip = value;
                    if (_selectedClip != null)
                    {
                        _selectedClip.IsSelected = true;
                        SyncClipToEngine(_selectedClip);
                    }
                    OnPropertyChanged();
                    OnPropertyChanged(nameof(HasSelectedClip));
                    OnPropertyChanged(nameof(IsTextSelected));
                    OnPropertyChanged(nameof(IsShapeSelected));
                    if (_engine != IntPtr.Zero)
                    {
                        NativeEngineApi.Engine_RenderFrame(_engine);
                    }
                }
            }
        }

        public bool HasSelectedClip => _selectedClip != null;
        public bool IsTextSelected => _selectedClip?.ElementType == 0;
        public bool IsShapeSelected => _selectedClip?.ElementType == 1 || _selectedClip?.ElementType == 2;

        public bool IsPlaying
        {
            get => _isPlaying;
            set { if (_isPlaying != value) { _isPlaying = value; OnPropertyChanged(); } }
        }

        public string MediaFilePath
        {
            get => _mediaFilePath;
            set { if (_mediaFilePath != value) { _mediaFilePath = value; OnPropertyChanged(); } }
        }

        public ObservableCollection<string> AvailableFonts { get; } = new();

        public MainViewModel()
        {
            InitializeTracks();
            PopulateDefaultFonts();
            SetupPlaybackTimer();
        }

        public void SetEngine(IntPtr engine)
        {
            _engine = engine;
            if (_engine != IntPtr.Zero)
            {
                LoadDirectWriteFonts();
            }
        }

        private void InitializeTracks()
        {
            _timeline.Tracks.Add(new TrackModel { Id = 1, Name = "视频主轨", TrackType = TrackType.Video, Icon = "Video" });
            _timeline.Tracks.Add(new TrackModel { Id = 2, Name = "文字字幕轨", TrackType = TrackType.Text, Icon = "Font" });
            _timeline.Tracks.Add(new TrackModel { Id = 3, Name = "图形覆层轨", TrackType = TrackType.Shape, Icon = "Shapes" });
        }

        private void PopulateDefaultFonts()
        {
            string[] defaultFonts = { "Segoe UI", "Microsoft YaHei", "Arial", "Impact", "SimHei", "KaiTi", "Consolas", "Verdana" };
            foreach (var f in defaultFonts)
            {
                if (!AvailableFonts.Contains(f)) AvailableFonts.Add(f);
            }
        }

        private void LoadDirectWriteFonts()
        {
            try
            {
                NativeEngineApi.Engine_EnumerateFonts((fontName, userData) =>
                {
                    if (!string.IsNullOrWhiteSpace(fontName) && !AvailableFonts.Contains(fontName))
                    {
                        AvailableFonts.Add(fontName);
                    }
                }, IntPtr.Zero);
            }
            catch
            {
                // Fallback already populated
            }
        }

        private void SetupPlaybackTimer()
        {
            _playbackTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(33) };
            _playbackTimer.Tick += (s, e) =>
            {
                if (_engine != IntPtr.Zero && _isPlaying)
                {
                    long curPos = NativeEngineApi.Engine_GetCurrentPositionMs(_engine);
                    _timeline.CurrentTimeMs = curPos;

                    if (curPos >= _timeline.TotalDurationMs)
                    {
                        Pause();
                    }
                }
            };
        }

        public bool LoadMedia(string filePath)
        {
            if (_engine == IntPtr.Zero || string.IsNullOrEmpty(filePath)) return false;

            bool success = NativeEngineApi.Engine_LoadMedia(_engine, filePath);
            if (success)
            {
                MediaFilePath = filePath;
                long dur = NativeEngineApi.Engine_GetDurationMs(_engine);
                if (dur > 0)
                {
                    _timeline.TotalDurationMs = dur;
                }

                // Add or update clip in Video track
                var videoTrack = _timeline.Tracks.FirstOrDefault(t => t.TrackType == TrackType.Video);
                if (videoTrack != null)
                {
                    videoTrack.Clips.Clear();
                    var videoClip = new ClipModel
                    {
                        Id = 9999,
                        Title = System.IO.Path.GetFileName(filePath),
                        TrackType = TrackType.Video,
                        StartTimeMs = 0,
                        DurationMs = _timeline.TotalDurationMs
                    };
                    videoTrack.Clips.Add(videoClip);
                }

                _timeline.CurrentTimeMs = 0;
                SeekTo(0);
            }
            return success;
        }

        public void TogglePlayPause()
        {
            if (IsPlaying) Pause();
            else Play();
        }

        public void Play()
        {
            if (_engine != IntPtr.Zero)
            {
                NativeEngineApi.Engine_Play(_engine);
                IsPlaying = true;
                _playbackTimer?.Start();
            }
        }

        public void Pause()
        {
            if (_engine != IntPtr.Zero)
            {
                NativeEngineApi.Engine_Pause(_engine);
                IsPlaying = false;
                _playbackTimer?.Stop();
            }
        }

        public void SeekTo(long timeMs)
        {
            _timeline.CurrentTimeMs = timeMs;
            if (_engine != IntPtr.Zero)
            {
                NativeEngineApi.Engine_Seek(_engine, timeMs);
            }
        }

        public ClipModel AddTextClip()
        {
            var textTrack = _timeline.Tracks.FirstOrDefault(t => t.TrackType == TrackType.Text);
            int id = _nextElementId++;
            int count = textTrack?.Clips.Count ?? 0;
            float posX = 160.0f + (count % 5) * 30.0f;
            float posY = 220.0f + (count % 5) * 50.0f;

            string defaultText = "请输入字幕文本";
            float initialFontSize = 36.0f;
            float initialWidth = Math.Max(180.0f, defaultText.Length * initialFontSize);
            float initialHeight = initialFontSize * 1.5f;

            var clip = new ClipModel
            {
                Id = id,
                Title = $"字幕 {id}",
                TrackType = TrackType.Text,
                ElementType = 0,
                StartTimeMs = _timeline.CurrentTimeMs,
                DurationMs = 3000,
                X = posX,
                Y = posY,
                Width = initialWidth,
                Height = initialHeight,
                FillColor = Color.FromArgb(255, 255, 255, 255),
                StrokeColor = Color.FromArgb(255, 0, 0, 0),
                StrokeWidth = 2.0f,
                FontSize = initialFontSize,
                FontFamily = "Microsoft YaHei UI",
                ContentText = defaultText
            };

            textTrack?.Clips.Add(clip);
            SelectedClip = clip;
            return clip;
        }

        public ClipModel AddRectangleClip()
        {
            var shapeTrack = _timeline.Tracks.FirstOrDefault(t => t.TrackType == TrackType.Shape);
            int id = _nextElementId++;
            int count = shapeTrack?.Clips.Count ?? 0;
            float posX = 240.0f + (count % 5) * 30.0f;
            float posY = 200.0f + (count % 5) * 40.0f;

            var clip = new ClipModel
            {
                Id = id,
                Title = $"矩形 {id}",
                TrackType = TrackType.Shape,
                ElementType = 1,
                StartTimeMs = _timeline.CurrentTimeMs,
                DurationMs = 3000,
                X = posX,
                Y = posY,
                Width = 280,
                Height = 160,
                FillColor = Color.FromArgb(180, 0, 120, 215), // Accent semi-transparent
                StrokeColor = Color.FromArgb(255, 255, 255, 255),
                StrokeWidth = 3.0f
            };

            shapeTrack?.Clips.Add(clip);
            SelectedClip = clip;
            return clip;
        }

        public ClipModel AddCircleClip()
        {
            var shapeTrack = _timeline.Tracks.FirstOrDefault(t => t.TrackType == TrackType.Shape);
            int id = _nextElementId++;
            int count = shapeTrack?.Clips.Count ?? 0;
            float posX = 280.0f + (count % 5) * 30.0f;
            float posY = 200.0f + (count % 5) * 40.0f;

            var clip = new ClipModel
            {
                Id = id,
                Title = $"圆形 {id}",
                TrackType = TrackType.Shape,
                ElementType = 2,
                StartTimeMs = _timeline.CurrentTimeMs,
                DurationMs = 3000,
                X = posX,
                Y = posY,
                Width = 180,
                Height = 180,
                FillColor = Color.FromArgb(180, 230, 80, 80),
                StrokeColor = Color.FromArgb(255, 255, 255, 255),
                StrokeWidth = 3.0f
            };

            shapeTrack?.Clips.Add(clip);
            SelectedClip = clip;
            return clip;
        }

        public void DeleteSelectedClip()
        {
            if (_selectedClip == null) return;

            var clipToDelete = _selectedClip;
            _selectedClip = null;

            foreach (var track in _timeline.Tracks)
            {
                if (track.Clips.Contains(clipToDelete))
                {
                    track.Clips.Remove(clipToDelete);
                    break;
                }
            }

            if (_engine != IntPtr.Zero)
            {
                NativeEngineApi.Engine_RemoveElement(_engine, clipToDelete.Id);
                NativeEngineApi.Engine_RenderFrame(_engine);
            }

            OnPropertyChanged(nameof(SelectedClip));
            OnPropertyChanged(nameof(HasSelectedClip));
            OnPropertyChanged(nameof(IsTextSelected));
            OnPropertyChanged(nameof(IsShapeSelected));
        }

        public void SyncClipToEngine(ClipModel clip)
        {
            if (_engine == IntPtr.Zero || clip.TrackType == TrackType.Video) return;

            var desc = clip.ToNativeDesc();
            NativeEngineApi.Engine_UpsertElement(_engine, ref desc);
        }

        public void SelectClipById(int id)
        {
            foreach (var track in _timeline.Tracks)
            {
                var found = track.Clips.FirstOrDefault(c => c.Id == id);
                if (found != null)
                {
                    SelectedClip = found;
                    SeekTo(found.StartTimeMs);
                    return;
                }
            }
        }
    }
}
