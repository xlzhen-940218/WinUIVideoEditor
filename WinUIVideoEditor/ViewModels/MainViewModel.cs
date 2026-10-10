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
                    OnPropertyChanged(nameof(IsAudioSelected));
                    if (_engine != IntPtr.Zero)
                    {
                        NativeEngineApi.Engine_RenderFrame(_engine);
                    }
                }
            }
        }

        public bool HasSelectedClip => _selectedClip != null;
        public bool IsTextSelected => _selectedClip != null && _selectedClip.TrackType == TrackType.Text;
        public bool IsShapeSelected => _selectedClip != null && _selectedClip.TrackType == TrackType.Shape;
        public bool IsAudioSelected => _selectedClip != null && _selectedClip.TrackType == TrackType.Audio;

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
            if (_engine == IntPtr.Zero)
            {
                _playbackTimer?.Stop();
                IsPlaying = false;
                return;
            }
            LoadDirectWriteFonts();
        }

        private void InitializeTracks()
        {
            _timeline.Tracks.Add(new TrackModel { Id = 1, Name = "视频主轨", TrackType = TrackType.Video, Icon = "Video" });
            _timeline.Tracks.Add(new TrackModel { Id = 2, Name = "音频轨道", TrackType = TrackType.Audio, Icon = "Audio" });
            _timeline.Tracks.Add(new TrackModel { Id = 3, Name = "文字字幕轨", TrackType = TrackType.Text, Icon = "Font" });
            _timeline.Tracks.Add(new TrackModel { Id = 4, Name = "图形覆层轨", TrackType = TrackType.Shape, Icon = "Shapes" });
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
            if (string.IsNullOrEmpty(filePath)) return false;

            string ext = System.IO.Path.GetExtension(filePath).ToLowerInvariant();
            if (ext == ".mp3" || ext == ".wav" || ext == ".aac" || ext == ".flac" || ext == ".m4a" || ext == ".ogg" || ext == ".wma")
            {
                return LoadAudio(filePath);
            }

            if (_engine == IntPtr.Zero) return false;

            bool success = NativeEngineApi.Engine_LoadMedia(_engine, filePath);
            if (success)
            {
                MediaFilePath = filePath;
                long dur = NativeEngineApi.Engine_GetLoadedMediaDurationMs(_engine);
                if (dur <= 0) dur = 10000;

                long clipStartMs = 0;
                var videoTrack = _timeline.Tracks.FirstOrDefault(t => t.TrackType == TrackType.Video);
                if (videoTrack != null)
                {
                    if (videoTrack.Clips.Count == 0)
                    {
                        clipStartMs = 0;
                        var videoClip = new ClipModel
                        {
                            Id = _nextElementId++,
                            Title = System.IO.Path.GetFileName(filePath),
                            TrackType = TrackType.Video,
                            StartTimeMs = 0,
                            DurationMs = dur,
                            ContentText = filePath
                        };
                        videoTrack.Clips.Add(videoClip);
                        _timeline.TotalDurationMs = dur;
                        _timeline.CurrentTimeMs = 0;
                    }
                    else
                    {
                        clipStartMs = _timeline.TotalDurationMs;
                        var videoClip = new ClipModel
                        {
                            Id = _nextElementId++,
                            Title = System.IO.Path.GetFileName(filePath),
                            TrackType = TrackType.Video,
                            StartTimeMs = clipStartMs,
                            DurationMs = dur,
                            ContentText = filePath
                        };
                        videoTrack.Clips.Add(videoClip);
                        _timeline.TotalDurationMs = clipStartMs + dur;
                    }

                    SyncVideoClipsToEngine();
                }

                // Extract audio waveform and add audio clip to Audio Track
                float[] waveform = new float[500];
                int sampleCount = NativeEngineApi.Engine_ExtractAudioWaveform(filePath, waveform, waveform.Length);
                float[]? finalWaveform = null;
                if (sampleCount > 0)
                {
                    finalWaveform = new float[sampleCount];
                    Array.Copy(waveform, finalWaveform, sampleCount);
                }

                var audioTrack = _timeline.Tracks.FirstOrDefault(t => t.TrackType == TrackType.Audio);
                if (audioTrack != null)
                {
                    var audioClip = new ClipModel
                    {
                        Id = _nextElementId++,
                        Title = System.IO.Path.GetFileName(filePath) + " (音频)",
                        TrackType = TrackType.Audio,
                        StartTimeMs = clipStartMs,
                        DurationMs = dur,
                        ContentText = filePath,
                        WaveformData = finalWaveform
                    };
                    audioTrack.Clips.Add(audioClip);
                    SyncAudioClipsToEngine();
                }

                // Show first frame immediately without having to click play
                SeekTo(0);
                NativeEngineApi.Engine_RenderFrame(_engine);
            }
            return success;
        }

        public bool LoadAudio(string filePath)
        {
            if (string.IsNullOrEmpty(filePath)) return false;

            long dur = NativeEngineApi.Engine_GetAudioDurationMs(filePath);
            if (dur <= 0) dur = 10000;

            float[] waveform = new float[500];
            int sampleCount = NativeEngineApi.Engine_ExtractAudioWaveform(filePath, waveform, waveform.Length);
            float[]? finalWaveform = null;
            if (sampleCount > 0)
            {
                finalWaveform = new float[sampleCount];
                Array.Copy(waveform, finalWaveform, sampleCount);
            }

            var audioTrack = _timeline.Tracks.FirstOrDefault(t => t.TrackType == TrackType.Audio);
            if (audioTrack != null)
            {
                long startMs = audioTrack.Clips.Count == 0 ? 0 : audioTrack.Clips.Max(c => c.EndTimeMs);
                var audioClip = new ClipModel
                {
                    Id = _nextElementId++,
                    Title = System.IO.Path.GetFileName(filePath),
                    TrackType = TrackType.Audio,
                    StartTimeMs = startMs,
                    DurationMs = dur,
                    ContentText = filePath,
                    WaveformData = finalWaveform
                };
                audioTrack.Clips.Add(audioClip);

                if (audioClip.EndTimeMs > _timeline.TotalDurationMs)
                {
                    _timeline.TotalDurationMs = audioClip.EndTimeMs;
                }

                SyncAudioClipsToEngine();
                return true;
            }
            return false;
        }

        public void SyncVideoClipsToEngine()
        {
            if (_engine == IntPtr.Zero) return;
            NativeEngineApi.Engine_ClearVideoClips(_engine);
            var videoTrack = _timeline.Tracks.FirstOrDefault(t => t.TrackType == TrackType.Video);
            if (videoTrack != null)
            {
                foreach (var clip in videoTrack.Clips)
                {
                    NativeEngineApi.Engine_AddVideoClip(_engine, clip.ContentText, clip.StartTimeMs, clip.DurationMs);
                }
            }
        }

        public void SyncAudioClipsToEngine()
        {
            if (_engine == IntPtr.Zero) return;
            NativeEngineApi.Engine_ClearAudioClips(_engine);
            var audioTrack = _timeline.Tracks.FirstOrDefault(t => t.TrackType == TrackType.Audio);
            if (audioTrack != null)
            {
                foreach (var clip in audioTrack.Clips)
                {
                    NativeEngineApi.Engine_AddAudioClip(_engine, clip.ContentText, clip.StartTimeMs, clip.DurationMs);
                }
            }
        }

        public void TogglePlayPause()
        {
            if (!Timeline.CanPlay) return;
            if (IsPlaying) Pause();
            else Play();
        }

        public void Play()
        {
            if (!Timeline.CanPlay || _engine == IntPtr.Zero) return;
            NativeEngineApi.Engine_Play(_engine);
            IsPlaying = true;
            _playbackTimer?.Start();
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

        public ClipModel AddTextClip(string initialText = "请输入字幕文本")
        {
            var textTrack = _timeline.Tracks.FirstOrDefault(t => t.TrackType == TrackType.Text);
            int id = _nextElementId++;
            int count = textTrack?.Clips.Count ?? 0;
            float posX = 160.0f + (count % 5) * 30.0f;
            float posY = 220.0f + (count % 5) * 50.0f;

            string defaultText = string.IsNullOrWhiteSpace(initialText) ? "请输入字幕文本" : initialText;
            float initialFontSize = 36.0f;
            float initialWidth = Math.Max(180.0f, defaultText.Length * initialFontSize);
            float initialHeight = initialFontSize * 1.5f;

            var clip = new ClipModel
            {
                Id = id,
                Title = defaultText.Length > 8 ? defaultText.Substring(0, 8) + "..." : defaultText,
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

        public ClipModel AddTriangleClip()
        {
            var shapeTrack = _timeline.Tracks.FirstOrDefault(t => t.TrackType == TrackType.Shape);
            int id = _nextElementId++;
            int count = shapeTrack?.Clips.Count ?? 0;
            float posX = 260.0f + (count % 5) * 30.0f;
            float posY = 200.0f + (count % 5) * 40.0f;

            var clip = new ClipModel
            {
                Id = id,
                Title = $"三角形 {id}",
                TrackType = TrackType.Shape,
                ElementType = 3,
                StartTimeMs = _timeline.CurrentTimeMs,
                DurationMs = 3000,
                X = posX,
                Y = posY,
                Width = 200,
                Height = 180,
                FillColor = Color.FromArgb(180, 245, 166, 35),
                StrokeColor = Color.FromArgb(255, 255, 255, 255),
                StrokeWidth = 3.0f
            };

            shapeTrack?.Clips.Add(clip);
            SelectedClip = clip;
            return clip;
        }

        public ClipModel AddStarClip()
        {
            var shapeTrack = _timeline.Tracks.FirstOrDefault(t => t.TrackType == TrackType.Shape);
            int id = _nextElementId++;
            int count = shapeTrack?.Clips.Count ?? 0;
            float posX = 260.0f + (count % 5) * 30.0f;
            float posY = 200.0f + (count % 5) * 40.0f;

            var clip = new ClipModel
            {
                Id = id,
                Title = $"五角星 {id}",
                TrackType = TrackType.Shape,
                ElementType = 4,
                StartTimeMs = _timeline.CurrentTimeMs,
                DurationMs = 3000,
                X = posX,
                Y = posY,
                Width = 200,
                Height = 200,
                FillColor = Color.FromArgb(190, 255, 215, 0),
                StrokeColor = Color.FromArgb(255, 255, 255, 255),
                StrokeWidth = 3.0f
            };

            shapeTrack?.Clips.Add(clip);
            SelectedClip = clip;
            return clip;
        }

        public ClipModel AddArrowClip()
        {
            var shapeTrack = _timeline.Tracks.FirstOrDefault(t => t.TrackType == TrackType.Shape);
            int id = _nextElementId++;
            int count = shapeTrack?.Clips.Count ?? 0;
            float posX = 260.0f + (count % 5) * 30.0f;
            float posY = 200.0f + (count % 5) * 40.0f;

            var clip = new ClipModel
            {
                Id = id,
                Title = $"箭头 {id}",
                TrackType = TrackType.Shape,
                ElementType = 5,
                StartTimeMs = _timeline.CurrentTimeMs,
                DurationMs = 3000,
                X = posX,
                Y = posY,
                Width = 240,
                Height = 120,
                FillColor = Color.FromArgb(180, 76, 217, 100),
                StrokeColor = Color.FromArgb(255, 255, 255, 255),
                StrokeWidth = 3.0f
            };

            shapeTrack?.Clips.Add(clip);
            SelectedClip = clip;
            return clip;
        }

        public ClipModel AddDiamondClip()
        {
            var shapeTrack = _timeline.Tracks.FirstOrDefault(t => t.TrackType == TrackType.Shape);
            int id = _nextElementId++;
            int count = shapeTrack?.Clips.Count ?? 0;
            float posX = 260.0f + (count % 5) * 30.0f;
            float posY = 200.0f + (count % 5) * 40.0f;

            var clip = new ClipModel
            {
                Id = id,
                Title = $"菱形 {id}",
                TrackType = TrackType.Shape,
                ElementType = 6,
                StartTimeMs = _timeline.CurrentTimeMs,
                DurationMs = 3000,
                X = posX,
                Y = posY,
                Width = 180,
                Height = 180,
                FillColor = Color.FromArgb(180, 142, 68, 173),
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
                    if (track.TrackType == TrackType.Video)
                    {
                        SyncVideoClipsToEngine();
                    }
                    else if (track.TrackType == TrackType.Audio)
                    {
                        SyncAudioClipsToEngine();
                    }
                    break;
                }
            }

            if (_engine != IntPtr.Zero && clipToDelete.TrackType != TrackType.Video && clipToDelete.TrackType != TrackType.Audio)
            {
                NativeEngineApi.Engine_RemoveElement(_engine, clipToDelete.Id);
                NativeEngineApi.Engine_RenderFrame(_engine);
            }

            OnPropertyChanged(nameof(SelectedClip));
            OnPropertyChanged(nameof(HasSelectedClip));
            OnPropertyChanged(nameof(IsTextSelected));
            OnPropertyChanged(nameof(IsShapeSelected));
            OnPropertyChanged(nameof(IsAudioSelected));
        }

        public void SyncClipToEngine(ClipModel clip)
        {
            if (_engine == IntPtr.Zero || clip.TrackType == TrackType.Video) return;

            if (clip.TrackType == TrackType.Audio)
            {
                SyncAudioClipsToEngine();
                return;
            }

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
