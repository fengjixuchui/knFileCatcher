/*
 * MainViewModel.cs
 * Single owner of:
 *   - driver lifecycle (install/load on startup, unload/uninstall on exit)
 *   - FilterPort connection (driver client)
 *   - single backup reader (kernel-pushed messages drained on one C# task)
 *   - history (in-memory ObservableCollection updated as backups happen)
 *   - stats / process tree (polled via direct driver queries, no files)
 */

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Threading;

namespace KnFc.Ux;

public sealed class MainViewModel : INotifyPropertyChanged, IDisposable
{
    private readonly DriverClient   _client = new();
    private readonly NtPathMap      _drives = NtPathMap.Build();
    private readonly DispatcherTimer _timer;
    private BackupWorker?  _worker;
    private ManifestWriter _manifest = new();
    private bool _installedHere;
    private bool _sessionOpen;

    /* Pid -> node lookup for incremental updates driven by push events.
     * Touched only on the UI thread (Dispatcher.Invoke from worker callbacks
     * plus BuildTree from RefreshStats which already runs on UI).
     */
    private readonly Dictionary<long, ProcessNode> _nodesByPid = new();
    private bool _treeSeeded;
    private bool _loadedHere;
    private bool _bootstrapped;
    /* Single reader thread: multiple threads doing FilterGetMessage on the
     * same client port handle causes fltmgr to redeliver the same message
     * to each reader, so we'd see N copies of every backup. If file copy
     * latency becomes a bottleneck, switch to a producer/consumer split
     * with one reader and N copy workers reading from a managed queue. */
    private const int WorkerCount = 1;
    private const ulong DiskFreeMinBytes = 1UL << 30;  /* 1 GB */

    public MainViewModel()
    {
        BackupRoot   = ResolveDefaultBackupRoot();
        HistoryLimit = 256;

        History       = new ObservableCollection<HistoryEntry>();
        RootProcesses = new ObservableCollection<ProcessNode>();
        Sessions      = new ObservableCollection<SessionRow>();
        WatchRoots    = new ObservableCollection<string>();
        Excludes      = new ObservableCollection<string>();

        /* Re-evaluate CanExecute when the watch-root list changes. */
        WatchRoots.CollectionChanged += (_, _) => System.Windows.Input.CommandManager.InvalidateRequerySuggested();
        Excludes.CollectionChanged   += (_, _) => System.Windows.Input.CommandManager.InvalidateRequerySuggested();

        StartCommand            = new RelayCommand(_ => InvokeStart(), _ => CanStart);
        StopCommand             = new RelayCommand(_ => InvokeStop(),  _ => CanStop);
        AddWatchRootCommand     = new RelayCommand(_ => InvokeAddWatchRoot(),    _ => !string.IsNullOrWhiteSpace(NewWatchRoot));
        ClearWatchRootsCommand  = new RelayCommand(_ => InvokeClearRoots(),     _ => WatchRoots.Count > 0);
        RemoveWatchRootCommand  = new RelayCommand(p => InvokeRemoveRoot(p as string), p => p is string);
        AddExcludeCommand       = new RelayCommand(_ => InvokeAddExclude(),     _ => !string.IsNullOrWhiteSpace(NewExclude));
        ClearExcludesCommand    = new RelayCommand(_ => InvokeClearExcludes(),  _ => Excludes.Count > 0);
        RemoveExcludeCommand    = new RelayCommand(p => InvokeRemoveExclude(p as string), p => p is string);
        OpenSelectedCommand    = new RelayCommand(_ => OpenSelected(),     _ => Selected != null);
        OpenSessionCommand     = new RelayCommand(_ => OpenSession(),      _ => !string.IsNullOrEmpty(SessionDir));
        OpenSessionRowCommand  = new RelayCommand(_ => OpenSessionRow(),   _ => SelectedSession != null);
        RefreshNowCommand      = new RelayCommand(_ => RefreshStats());

        _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(1000) };
        _timer.Tick += (_, _) => RefreshStats();
    }

    /* ----- bootstrap -------------------------------------------------- */

    /// <summary>
    /// Register exclude patterns that protect our own bookkeeping files
    /// from being caught and re-backed-up. Substring matches against the
    /// kernel-side NT path (case-insensitive).
    /// </summary>
    private void RegisterSelfExcludes()
    {
        try
        {
            /* Manifest written by ManifestWriter for every backup. */
            _client.AddExclude("\\manifest.jsonl");
            /* The whole BackupRoot - anything underneath is ours by
             * definition and must never become a new backup source. */
            string nt = DosToNtSubstring(BackupRoot);
            if (!string.IsNullOrEmpty(nt))
            {
                _client.AddExclude(nt);
            }
        }
        catch
        {
            /* exclude registration failures are non-fatal */
        }
    }

    /// <summary>
    /// Convert "C:\path\to\dir" to a substring pattern that matches the
    /// kernel-side NT path "\Device\HarddiskVolumeN\path\to\dir\". Trailing
    /// backslash ensures we only match descendants, not unrelated siblings.
    /// </summary>
    private static string DosToNtSubstring(string dosPath)
    {
        if (string.IsNullOrEmpty(dosPath) || dosPath.Length < 2 || dosPath[1] != ':')
        {
            return "";
        }
        /* The kernel-side substring matcher already does case-insensitive
         * comparison and operates on the raw NT path. We don't need the
         * drive prefix - the unique tail "\path\to\dir\" is enough to
         * disambiguate. */
        string tail = dosPath.Substring(2);
        if (!tail.EndsWith("\\"))
        {
            tail += "\\";
        }
        return tail;
    }

    public bool Bootstrap(out string errorText)
    {
        errorText = "";
        int rc = DriverInstaller.EnsureInstalled(out _installedHere);
        if (rc != 0)
        {
            errorText = $"driver install failed (Win32 {rc}) - run as Administrator?";
            return false;
        }

        rc = DriverInstaller.LoadIfNeeded(out _loadedHere);
        if (rc != 0)
        {
            errorText = $"FilterLoad failed (Win32 {rc}) - testsigning + dev cert in place?";
            if (_installedHere) { DriverInstaller.Uninstall(); _installedHere = false; }
            return false;
        }

        rc = _client.Connect();
        if (rc != 0)
        {
            errorText = $"FilterConnect failed (Win32 {rc})";
            if (_loadedHere)    { DriverInstaller.Unload();    _loadedHere    = false; }
            if (_installedHere) { DriverInstaller.Uninstall(); _installedHere = false; }
            return false;
        }

        _bootstrapped = true;
        _timer.Start();
        RefreshStats();
        return true;
    }

    private bool EnsureSessionWorker(out string errorText)
    {
        errorText = "";
        if (_worker != null)
        {
            return true;
        }
        if (string.IsNullOrWhiteSpace(BackupRoot))
        {
            errorText = "backup root is empty";
            return false;
        }

        string stamp = DateTime.UtcNow.ToString("yyyyMMdd'T'HHmmss'Z'");
        SessionDir   = Path.Combine(BackupRoot, "session_" + stamp);

        ManifestPath = Path.Combine(SessionDir, "manifest.jsonl");
        try
        {
            Directory.CreateDirectory(SessionDir);
        }
        catch
        {
            errorText = "session directory create failed: " + SessionDir;
            return false;
        }
        if (!_manifest.Open(ManifestPath))
        {
            errorText = "manifest open failed: " + ManifestPath;
            return false;
        }

        /* Self-feedback guards must be installed before workers start
         * draining messages and before the driver begins tracking. The
         * manifest open above is safe because tracking is still idle.
         */
        RegisterSelfExcludes();

        /* Spin up backup workers */
        _worker = new BackupWorker(_client.Port, SessionDir, _manifest);
        _worker.OnBackup       = OnBackupArrived;
        _worker.OnProcessEvent = OnProcessEventArrived;
        _worker.Start(WorkerCount);

        _sessionOpen = true;
        OnPropertyChanged(nameof(CanEditBackupRoot));
        OnPropertyChanged(nameof(BackupRootReadOnly));
        return true;
    }

    public void Shutdown()
    {
        _timer.Stop();
        if (_worker != null) { _worker.Stop(); _worker.Dispose(); _worker = null; }
        _manifest.Dispose();
        _client.Dispose();
        if (_loadedHere)    { DriverInstaller.Unload();    _loadedHere    = false; }
        if (_installedHere) { DriverInstaller.Uninstall(); _installedHere = false; }
    }

    public void Dispose() => Shutdown();

    /* ----- commands --------------------------------------------------- */

    private void Invoke(string label, Func<DriverClient, int> action)
    {
        if (!_client.IsConnected) { LastReply = $"{label}: not connected"; return; }
        int rc = action(_client);
        LastReply = rc == 0 ? $"ok  {label}" : $"FAIL {label} rc={rc}";
        RefreshStats();
    }

    public bool CanStart => _client.IsConnected
        && Active == 0
        && WatchRoots.Count > 0
        && !string.IsNullOrWhiteSpace(BackupRoot);
    public bool CanStop  => _client.IsConnected && Active != 0;

    private void InvokeStart()
    {
        if (!EnsureSessionWorker(out string errorText))
        {
            LastReply = "FAIL tracking-start  " + errorText;
            RefreshStats();
            return;
        }

        int rc = _client.SendHeader(MsgType.Start);
        LastReply = rc == 0 ? "ok  tracking-start" : $"FAIL tracking-start rc={rc}";
        RefreshStats();
    }

    private void InvokeStop()
    {
        int rc = _client.SendHeader(MsgType.Stop);
        LastReply = rc == 0 ? "ok  tracking-stop" : $"FAIL tracking-stop rc={rc}";
        RefreshStats();
    }

    private void InvokeAddWatchRoot()
    {
        if (string.IsNullOrWhiteSpace(NewWatchRoot)) { return; }
        string dos = NewWatchRoot.Trim();
        string ntVolume = DosToNt(dos);                       /* \Device\HarddiskVolumeN\... */
        string ntDosNs  = "\\??\\" + dos.TrimStart('\\');     /* \??\C:\...                  */

        if (string.IsNullOrEmpty(ntVolume))
        {
            LastReply = $"FAIL add-watch-root - DosToNt('{dos}')";
            return;
        }

        /* Register BOTH namespaces: snapshot/CreateInfo paths use
         * \Device\HarddiskVolumeN, but PsCreateProcessNotify often
         * delivers \??\C:\... - registering both makes prefix matching
         * work either way.
         */
        int rcVol = _client.AddWatchRoot(ntVolume);
        int rcDos = _client.AddWatchRoot(ntDosNs);

        if (rcVol == 0 || rcDos == 0)
        {
            if (!WatchRoots.Contains(dos)) { WatchRoots.Add(dos); }
            NewWatchRoot = "";
            LastReply = $"ok  add-watch-root  {dos}  ->  [{ntVolume}] + [{ntDosNs}]";
        }
        else
        {
            LastReply = $"FAIL add-watch-root  vol_rc={rcVol} dos_rc={rcDos}";
        }
        RefreshStats();
    }

    private void InvokeClearRoots()
    {
        int rc = _client.SendHeader(MsgType.ClearWatchRoots);
        if (rc == 0) { WatchRoots.Clear(); }
        LastReply = rc == 0 ? "ok  clear-watch-roots" : $"FAIL clear-watch-roots rc={rc}";
        RefreshStats();
    }

    /// <summary>
    /// Remove one watch root. The driver has no per-item remove API,
    /// so we re-publish the truncated list: ClearWatchRoots + re-add
    /// everything except the dropped entry.
    /// </summary>
    private void InvokeRemoveRoot(string? dos)
    {
        if (string.IsNullOrEmpty(dos) || !WatchRoots.Contains(dos)) { return; }

        int rc = _client.SendHeader(MsgType.ClearWatchRoots);
        if (rc != 0)
        {
            LastReply = $"FAIL remove-watch-root  clear rc={rc}";
            return;
        }

        WatchRoots.Remove(dos);

        int reAddFails = 0;
        foreach (var d in WatchRoots.ToList())
        {
            string ntVolume = DosToNt(d);
            string ntDosNs  = "\\??\\" + d.TrimStart('\\');
            if (string.IsNullOrEmpty(ntVolume))
            {
                ++reAddFails;
                continue;
            }
            int rcVol = _client.AddWatchRoot(ntVolume);
            int rcDos = _client.AddWatchRoot(ntDosNs);
            if (rcVol != 0 && rcDos != 0) { ++reAddFails; }
        }

        LastReply = (reAddFails == 0)
            ? $"ok  remove-watch-root  {dos}"
            : $"WARN remove-watch-root  {dos}  (re-add failed for {reAddFails} item(s); list may be out of sync)";
        RefreshStats();
    }

    private void InvokeAddExclude()
    {
        if (string.IsNullOrWhiteSpace(NewExclude)) { return; }
        string pat = NewExclude.Trim();
        int rc = _client.AddExclude(pat);
        if (rc == 0)
        {
            if (!Excludes.Contains(pat)) { Excludes.Add(pat); }
            NewExclude = "";
            LastReply = $"ok  add-exclude  {pat}";
        }
        else
        {
            LastReply = $"FAIL add-exclude rc={rc}";
        }
        RefreshStats();
    }

    private void InvokeClearExcludes()
    {
        int rc = _client.SendHeader(MsgType.ClearExcludes);
        if (rc == 0)
        {
            Excludes.Clear();
            if (_sessionOpen)
            {
                RegisterSelfExcludes();
            }
        }
        LastReply = rc == 0 ? "ok  clear-excludes" : $"FAIL clear-excludes rc={rc}";
        RefreshStats();
    }

    /// <summary>
    /// Same trick as InvokeRemoveRoot - driver has no per-item remove.
    /// </summary>
    private void InvokeRemoveExclude(string? pat)
    {
        if (string.IsNullOrEmpty(pat) || !Excludes.Contains(pat)) { return; }

        int rc = _client.SendHeader(MsgType.ClearExcludes);
        if (rc != 0)
        {
            LastReply = $"FAIL remove-exclude  clear rc={rc}";
            return;
        }

        Excludes.Remove(pat);
        if (_sessionOpen)
        {
            RegisterSelfExcludes();
        }

        int reAddFails = 0;
        foreach (var p in Excludes.ToList())
        {
            if (_client.AddExclude(p) != 0) { ++reAddFails; }
        }

        LastReply = (reAddFails == 0)
            ? $"ok  remove-exclude  {pat}"
            : $"WARN remove-exclude  {pat}  (re-add failed for {reAddFails} pattern(s); list may be out of sync)";
        RefreshStats();
    }

    private string DosToNt(string dos)
    {
        if (dos.Length < 2 || dos[1] != ':') { return ""; }
        var sb = new System.Text.StringBuilder(260);
        if (Native.QueryDosDevice(dos.Substring(0, 2), sb, (uint)sb.Capacity) == 0)
        {
            return "";
        }
        return sb.ToString() + dos.Substring(2);
    }

    /* ----- bindable properties ---------------------------------------- */

    private string _backupRoot = "";
    public string BackupRoot
    {
        get => _backupRoot;
        set
        {
            _backupRoot = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(CanStart));
            System.Windows.Input.CommandManager.InvalidateRequerySuggested();
        }
    }
    public bool CanEditBackupRoot => !_sessionOpen;
    public bool BackupRootReadOnly => _sessionOpen;

    /// <summary>
    /// Defaults to "<exe-dir>\backup" so a fresh install just works without
    /// the user picking a path. The directory is created on demand.
    /// Falls back to %LOCALAPPDATA%\knFileCatcher\backup if the EXE folder
    /// is read-only (e.g. extracted under Program Files).
    /// </summary>
    private static string ResolveDefaultBackupRoot()
    {
        string exe = Environment.ProcessPath ?? AppContext.BaseDirectory;
        string? dir = Path.GetDirectoryName(exe);
        if (string.IsNullOrEmpty(dir))
        {
            dir = AppContext.BaseDirectory;
        }

        string candidate = Path.Combine(dir!, "backup");
        try
        {
            Directory.CreateDirectory(candidate);
            /* Write probe - if this fails we landed in a restricted dir
             * like Program Files. Bail to LocalAppData. */
            string probe = Path.Combine(candidate, ".knfc-write-probe");
            File.WriteAllText(probe, "");
            File.Delete(probe);
            return candidate;
        }
        catch
        {
            string fallback = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "knFileCatcher", "backup");
            try
            {
                Directory.CreateDirectory(fallback);
            }
            catch
            {
                /* If even LocalAppData fails, return the original
                 * candidate string so the user sees something sensible
                 * and can fix it manually. */
            }
            return fallback;
        }
    }

    public int HistoryLimit { get; set; }

    private string _newWatchRoot = "";
    public string NewWatchRoot { get => _newWatchRoot; set { _newWatchRoot = value; OnPropertyChanged(); } }

    private string _newExclude = "";
    public string NewExclude { get => _newExclude; set { _newExclude = value; OnPropertyChanged(); } }

    private string _sessionDir = "";
    public string SessionDir { get => _sessionDir; set { _sessionDir = value; OnPropertyChanged(); } }

    private string _manifestPath = "";
    public string ManifestPath { get => _manifestPath; set { _manifestPath = value; OnPropertyChanged(); } }

    /* stats */
    private int  _active;
    public int Active
    {
        get => _active;
        set
        {
            if (_active != value)
            {
                _active = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(ActiveText));
                OnPropertyChanged(nameof(CanStart));
                OnPropertyChanged(nameof(CanStop));
                System.Windows.Input.CommandManager.InvalidateRequerySuggested();
            }
        }
    }
    public string ActiveText => _active != 0 ? "tracking" : "idle";
    private int  _trackedProcesses;  public int TrackedProcesses { get => _trackedProcesses; set { _trackedProcesses = value; OnPropertyChanged(); } }
    private int  _watchRootCount;    public int WatchRootCount   { get => _watchRootCount;   set { _watchRootCount   = value; OnPropertyChanged(); } }
    private int  _excludeCount;      public int ExcludeCount     { get => _excludeCount;     set { _excludeCount     = value; OnPropertyChanged(); } }
    private long _queueDepth;        public long QueueDepth      { get => _queueDepth;       set { _queueDepth       = value; OnPropertyChanged(); } }
    private long _queueSent;         public long QueueSent       { get => _queueSent;        set { _queueSent        = value; OnPropertyChanged(); } }
    private long _queueFailed;       public long QueueFailed     { get => _queueFailed;      set { _queueFailed      = value; OnPropertyChanged(); } }
    private long _queueDropped;      public long QueueDropped    { get => _queueDropped;     set { _queueDropped     = value; OnPropertyChanged(); } }
    private long _excludeMatched;    public long ExcludeMatched  { get => _excludeMatched;   set { _excludeMatched   = value; OnPropertyChanged(); } }
    private long _syncSent;          public long SyncCleanupSent { get => _syncSent;         set { _syncSent         = value; OnPropertyChanged(); } }
    private long _syncFailed;        public long SyncCleanupFailed{ get => _syncFailed;      set { _syncFailed       = value; OnPropertyChanged(); } }

    /* svc counters (now produced by BackupWorker locally) */
    public long Backed       => _worker?.Backed       ?? 0;
    public long Failed       => _worker?.Failed       ?? 0;
    public long Refused      => _worker?.Refused      ?? 0;
    public long ProcEventsRx => _worker?.ProcEventsRx ?? 0;
    public long UnknownRx    => _worker?.UnknownRx    ?? 0;
    public long DupRx        => _worker?.DupRx        ?? 0;
    public string WorkerLastError => _worker?.LastError ?? "";
    private bool _diskFull;
    public bool DiskFull { get => _diskFull; set { _diskFull = value; if (_worker != null) _worker.DiskFull = value; OnPropertyChanged(); OnPropertyChanged(nameof(DiskFullText)); } }
    public string DiskFullText => _diskFull ? "FULL" : "ok";
    public long DiskFreeMin => (long)DiskFreeMinBytes;
    private string _ts = ""; public string Ts { get => _ts; set { _ts = value; OnPropertyChanged(); } }

    public ObservableCollection<string> WatchRoots { get; }
    public ObservableCollection<string> Excludes   { get; }

    public ObservableCollection<HistoryEntry> History { get; }
    private HistoryEntry? _selected;
    public  HistoryEntry? Selected { get => _selected; set { _selected = value; OnPropertyChanged(); } }

    public ObservableCollection<ProcessNode> RootProcesses { get; }
    private int _treeCount;     public int  TreeCount     { get => _treeCount;     set { _treeCount     = value; OnPropertyChanged(); } }
    private bool _treeTruncated; public bool TreeTruncated { get => _treeTruncated; set { _treeTruncated = value; OnPropertyChanged(); } }

    public ObservableCollection<SessionRow> Sessions { get; }
    private SessionRow? _selectedSession;
    public  SessionRow? SelectedSession { get => _selectedSession; set { _selectedSession = value; OnPropertyChanged(); } }

    private string _lastReply = "";
    public  string LastReply { get => _lastReply; set { _lastReply = value; OnPropertyChanged(); } }

    /* commands */
    public RelayCommand StartCommand            { get; }
    public RelayCommand StopCommand             { get; }
    public RelayCommand AddWatchRootCommand     { get; }
    public RelayCommand ClearWatchRootsCommand  { get; }
    public RelayCommand RemoveWatchRootCommand  { get; }
    public RelayCommand AddExcludeCommand       { get; }
    public RelayCommand ClearExcludesCommand    { get; }
    public RelayCommand RemoveExcludeCommand    { get; }
    public RelayCommand OpenSelectedCommand     { get; }
    public RelayCommand OpenSessionCommand      { get; }
    public RelayCommand OpenSessionRowCommand   { get; }
    public RelayCommand RefreshNowCommand       { get; }

    /* ----- behavior --------------------------------------------------- */

    private void RefreshStats()
    {
        if (!_bootstrapped || !_client.IsConnected) { return; }

        Ts = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ");

        if (_client.GetStats(out StatsWire st) == 0)
        {
            Active            = (int)st.Active;
            TrackedProcesses  = (int)st.TrackedProcessCount;
            WatchRootCount    = (int)st.WatchRootCount;
            ExcludeCount      = (int)st.ExcludeCount;
            QueueDepth        = (long)st.QueueDepth;
            QueueSent         = (long)st.QueueSent;
            QueueFailed       = (long)st.QueueSendFailed;
            QueueDropped      = (long)st.QueueDropped;
            ExcludeMatched    = (long)st.ExcludeMatched;
            SyncCleanupSent   = (long)st.SyncCleanupSent;
            SyncCleanupFailed = (long)st.SyncCleanupFailed;
        }

        /* Process tree is push-driven (KnFcMsgProcessEvent). The only
         * remaining poll is the one-shot snapshot below, which fires
         * exactly once after Start to seed entries that existed before
         * tracking was enabled. Subsequent insertions/exits arrive via
         * BackupWorker.OnProcessEvent -> ApplyProcessEvent. */
        if (!_treeSeeded)
        {
            if (_client.GetProcessTree(out var procs, out bool trunc) == 0)
            {
                BuildTree(procs);
                TreeCount     = procs.Count;
                TreeTruncated = trunc;
                _treeSeeded   = true;
            }
        }

        OnPropertyChanged(nameof(ProcEventsRx));
        OnPropertyChanged(nameof(UnknownRx));
        OnPropertyChanged(nameof(DupRx));
        OnPropertyChanged(nameof(WorkerLastError));
        OnPropertyChanged(nameof(Backed));
        OnPropertyChanged(nameof(Failed));
        OnPropertyChanged(nameof(Refused));

        UpdateDiskFull();
        SyncSessions();
    }

    private unsafe void BuildTree(List<ProcWire> procs)
    {
        var nodes = new List<ProcessNode>(procs.Count);
        for (int idx = 0; idx < procs.Count; ++idx)
        {
            ProcWire p = procs[idx];
            string image;
            unsafe
            {
                /* Clamp to the actual buffer size. A malformed reply
                 * with ImageLenChars > 120 would otherwise have us read
                 * past the fixed buffer into adjacent stack memory.
                 * BackupWorker's ProcessEvent path uses the same 120
                 * cap (see KNFC_TREE_IMAGE_CHARS in knFcProto.h). */
                int icch = (int)p.ImageLenChars;
                if (icch < 0) { icch = 0; }
                if (icch > 120) { icch = 120; }
                image = System.Runtime.InteropServices.Marshal.PtrToStringUni(
                    (IntPtr)(&p.Image[0]), icch);
            }
            nodes.Add(new ProcessNode
            {
                Pid           = (long)p.Pid,
                Ppid          = (long)p.ParentPid,
                Root          = (long)p.RootPid,
                Flags         = (int)p.Flags,
                ImageNt       = image,
                ImageFriendly = _drives.NtToFriendly(image)
            });
        }
        _nodesByPid.Clear();
        foreach (var n in nodes) { _nodesByPid[n.Pid] = n; }
        var roots = new List<ProcessNode>();
        foreach (var n in nodes)
        {
            if (n.Ppid != 0 && _nodesByPid.TryGetValue(n.Ppid, out var parent))
            {
                parent.Children.Add(n);
            }
            else
            {
                roots.Add(n);
            }
        }
        RootProcesses.Clear();
        foreach (var r in roots) { RootProcesses.Add(r); }
    }

    /* Push-event arrival from a BackupWorker thread.
     * Marshals onto the UI thread and performs an incremental tree update.
     */
    private void OnProcessEventArrived(ProcessEventNotify ev)
    {
        var app = Application.Current;
        if (app == null) { return; }
        app.Dispatcher.BeginInvoke(new Action(() => ApplyProcessEvent(ev)));
    }

    private void ApplyProcessEvent(ProcessEventNotify ev)
    {
        long pid  = (long)ev.Pid;
        long ppid = (long)ev.ParentPid;

        if (ev.EventType == ProcEventType.Created)
        {
            if (_nodesByPid.ContainsKey(pid))
            {
                return;
            }
            var node = new ProcessNode
            {
                Pid           = pid,
                Ppid          = ppid,
                Root          = (long)ev.RootPid,
                Flags         = (int)ev.Flags,
                ImageNt       = ev.Image,
                ImageFriendly = _drives.NtToFriendly(ev.Image)
            };
            _nodesByPid[pid] = node;

            /* Attach under the live parent if we have it, otherwise show
             * as a root so the user always sees it. ROOT-flagged nodes
             * are always top-level regardless of parent visibility.
             */
            bool asRoot = (node.Flags & 0x0001) != 0;
            if (!asRoot && ppid != 0 && _nodesByPid.TryGetValue(ppid, out var parent))
            {
                parent.Children.Add(node);
            }
            else
            {
                RootProcesses.Add(node);
            }
            TreeCount = _nodesByPid.Count;
        }
        else if (ev.EventType == ProcEventType.Exited)
        {
            if (!_nodesByPid.TryGetValue(pid, out var node))
            {
                return;
            }
            /* Keep the node in the tree so the user can still see what
             * ran. Just stamp KnFcTrackExited (0x4) so RoleText flips to
             * "[exited]". TreeCount stays unchanged on purpose - the
             * count reflects what was tracked, not what's alive.
             */
            node.Flags = node.Flags | 0x0004;
        }
    }

    private void OnBackupArrived(BackupEvent evt)
    {
        /* BeginInvoke (NOT Invoke) so the worker thread NEVER blocks on
         * the UI thread. A blocking Invoke deadlocks the worker if the
         * UI thread is busy (e.g. inside a long FilterSendMessage from
         * RefreshStats), and Queue sent stays at 1 while every subsequent
         * BackupRequest times out and SendFailed climbs. */
        var app = Application.Current;
        if (app == null)
        {
            return;
        }
        app.Dispatcher.BeginInvoke(new Action(() =>
        {
            var he = new HistoryEntry(
                Id:          (long)evt.Id,
                Ts:          evt.TsUtc.ToString("yyyy-MM-ddTHH:mm:ss.fffZ"),
                Pid:         (long)evt.Pid,
                RootPid:     (long)evt.RootPid,
                Flags:       (int)evt.Flags,
                Status:      evt.Status,
                SrcNt:       evt.SrcNt,
                SrcFriendly: _drives.NtToFriendly(evt.SrcNt),
                Dest:        evt.Dest);
            History.Insert(0, he);
            while (History.Count > HistoryLimit) { History.RemoveAt(History.Count - 1); }
        }));
    }

    private void UpdateDiskFull()
    {
        if (string.IsNullOrEmpty(BackupRoot)) { return; }
        try
        {
            var di = new DriveInfo(Path.GetPathRoot(BackupRoot) ?? "C:");
            DiskFull = (ulong)di.AvailableFreeSpace < DiskFreeMinBytes;
        }
        catch
        {
            /* ignore - not a fatal condition */
        }
    }

    private void SyncSessions()
    {
        if (string.IsNullOrEmpty(BackupRoot)) { return; }
        var fresh = SessionsService.Enumerate(BackupRoot, ManifestPath);
        bool same = Sessions.Count == fresh.Count;
        if (same)
        {
            for (int i = 0; i < fresh.Count; ++i)
            {
                if (!string.Equals(Sessions[i].Name, fresh[i].Name, StringComparison.Ordinal)
                    || Sessions[i].ManifestBytes != fresh[i].ManifestBytes)
                {
                    same = false;
                    break;
                }
            }
        }
        if (same) { return; }
        string? keep = SelectedSession?.Name;
        Sessions.Clear();
        foreach (var s in fresh) { Sessions.Add(s); }
        if (keep != null)
        {
            foreach (var s in Sessions)
            {
                if (s.Name == keep) { SelectedSession = s; break; }
            }
        }
    }

    private void OpenSelected()
    {
        if (Selected == null) { return; }
        string dest = Selected.Dest;
        if (string.IsNullOrEmpty(dest)) { return; }
        string args = File.Exists(dest)
            ? $"/select,\"{dest}\""
            : $"\"{Path.GetDirectoryName(dest)}\"";
        try { Process.Start(new ProcessStartInfo("explorer.exe", args) { UseShellExecute = true }); }
        catch (Exception ex) { LastReply = "explorer launch failed: " + ex.Message; }
    }

    private void OpenSession()
    {
        if (string.IsNullOrEmpty(SessionDir)) { return; }
        try { Process.Start(new ProcessStartInfo("explorer.exe", $"\"{SessionDir}\"") { UseShellExecute = true }); }
        catch (Exception ex) { LastReply = "explorer launch failed: " + ex.Message; }
    }

    private void OpenSessionRow()
    {
        if (SelectedSession == null) { return; }
        try { Process.Start(new ProcessStartInfo("explorer.exe", $"\"{SelectedSession.Path}\"") { UseShellExecute = true }); }
        catch (Exception ex) { LastReply = "explorer launch failed: " + ex.Message; }
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    private void OnPropertyChanged([CallerMemberName] string? name = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
