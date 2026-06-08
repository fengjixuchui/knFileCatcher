/*
 * StatsModel.cs
 * Domain types shared by the view: HistoryEntry (manifest row) and
 * NtPathMap (NT volume <-> drive letter).
 *
 * The stats.json / process-tree.json / manifest.jsonl polling that
 * used to live here is gone - knFcUxWpf now talks to the driver
 * directly and produces history events from BackupWorker.
 */

using System.Collections.Generic;

namespace KnFc.Ux;

public sealed record HistoryEntry(
    long     Id,
    string   Ts,
    long     Pid,
    long     RootPid,
    int      Flags,
    int      Status,
    string   SrcNt,
    string   SrcFriendly,
    string   Dest);

public sealed class NtPathMap
{
    private readonly List<(string Nt, char Letter)> _entries = new();

    public static NtPathMap Build()
    {
        var m = new NtPathMap();
        for (char L = 'A'; L <= 'Z'; ++L)
        {
            var buf = new System.Text.StringBuilder(260);
            uint n = Native.QueryDosDevice(L + ":", buf, (uint)buf.Capacity);
            if (n > 0 && buf.Length > 0)
            {
                m._entries.Add((buf.ToString(), L));
            }
        }
        return m;
    }

    public string NtToFriendly(string nt)
    {
        if (string.IsNullOrEmpty(nt) || nt[0] != '\\') { return nt; }
        foreach (var (ntDev, letter) in _entries)
        {
            if (nt.Length >= ntDev.Length
                && nt.StartsWith(ntDev, System.StringComparison.OrdinalIgnoreCase)
                && (nt.Length == ntDev.Length || nt[ntDev.Length] == '\\'))
            {
                return letter + ":" + nt[ntDev.Length..];
            }
        }
        return nt;
    }
}
