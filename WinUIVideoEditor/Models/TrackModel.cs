using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace WinUIVideoEditor.Models
{
    public class TrackModel : INotifyPropertyChanged
    {
        private int _id;
        private string _name = "轨道";
        private TrackType _trackType;
        private string _icon = "Video";
        private bool _isMuted;
        private bool _isLocked;

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

        public string Name
        {
            get => _name;
            set { if (_name != value) { _name = value; OnPropertyChanged(); } }
        }

        public TrackType TrackType
        {
            get => _trackType;
            set { if (_trackType != value) { _trackType = value; OnPropertyChanged(); } }
        }

        public string Icon
        {
            get => _icon;
            set { if (_icon != value) { _icon = value; OnPropertyChanged(); } }
        }

        public bool IsMuted
        {
            get => _isMuted;
            set { if (_isMuted != value) { _isMuted = value; OnPropertyChanged(); } }
        }

        public bool IsLocked
        {
            get => _isLocked;
            set { if (_isLocked != value) { _isLocked = value; OnPropertyChanged(); } }
        }

        public ObservableCollection<ClipModel> Clips { get; set; } = new();
    }
}
