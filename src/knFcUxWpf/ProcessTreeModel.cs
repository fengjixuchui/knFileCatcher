/*
 * ProcessTreeModel.cs
 * Display node for the Process Tree TreeView. Initial population is done
 * by MainViewModel.BuildTree from a one-shot GetProcessTree snapshot at
 * Start, then push events (OnProcessEventArrived) mutate this collection
 * incrementally. Exited processes are kept in the tree with their Flags
 * marked KnFcTrackExited so RoleText reports "[exited]".
 */

using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace KnFc.Ux;

public sealed class ProcessNode : INotifyPropertyChanged
{
    private int _flags;

    public long  Pid           { get; set; }
    public long  Ppid          { get; set; }
    public long  Root          { get; set; }
    public int   Flags
    {
        get => _flags;
        set
        {
            if (_flags == value)
            {
                return;
            }
            _flags = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(RoleText));
            OnPropertyChanged(nameof(IsExited));
        }
    }
    public string ImageNt      { get; set; } = "";
    public string ImageFriendly { get; set; } = "";

    public string DisplayName
    {
        get
        {
            string p = string.IsNullOrEmpty(ImageFriendly) ? ImageNt : ImageFriendly;
            int sep = p.LastIndexOfAny(new[] { '\\', '/' });
            return sep >= 0 ? p[(sep + 1)..] : p;
        }
    }

    public bool IsExited => (Flags & 0x0004) != 0;

    public string RoleText
    {
        get
        {
            bool isRoot  = (Flags & 0x0001) != 0;   /* KnFcTrackRoot  */
            bool isChild = (Flags & 0x0002) != 0;   /* KnFcTrackChild */
            bool snap    = (Flags & 0x0008) != 0;   /* KnFcTrackFromSnap */
            string baseTxt = isRoot ? "ROOT" : (isChild ? "CHILD" : "?");
            if (snap)
            {
                baseTxt += " (snap)";
            }
            if (IsExited)
            {
                baseTxt += " [exited]";
            }
            return baseTxt;
        }
    }

    public ObservableCollection<ProcessNode> Children { get; } = new();

    public event PropertyChangedEventHandler? PropertyChanged;

    private void OnPropertyChanged([CallerMemberName] string? name = null)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }
}
