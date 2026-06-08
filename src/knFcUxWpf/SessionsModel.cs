/*
 * SessionsModel.cs
 * Enumerate <backupRoot>\session_* directories. Each row carries a
 * manifest file-size; line counts are intentionally not computed
 * here because they require reading every byte and we don't want
 * to stall the UI thread.
 */

using System;
using System.Collections.Generic;
using System.IO;

namespace KnFc.Ux;

public sealed class SessionRow
{
    public string Name           { get; set; } = "";
    public string Path           { get; set; } = "";
    public DateTime LastWriteUtc { get; set; }
    public long ManifestBytes    { get; set; }
    public bool ManifestPresent  { get; set; }
    public bool IsActive         { get; set; }
}

public static class SessionsService
{
    public static List<SessionRow> Enumerate(string backupRoot, string? activeManifestPath)
    {
        var rows = new List<SessionRow>();
        if (string.IsNullOrWhiteSpace(backupRoot) || !Directory.Exists(backupRoot))
        {
            return rows;
        }
        DirectoryInfo[] dirs;
        try
        {
            var di = new DirectoryInfo(backupRoot);
            dirs = di.GetDirectories("session_*");
        }
        catch
        {
            return rows;
        }
        foreach (var d in dirs)
        {
            string manifest = System.IO.Path.Combine(d.FullName, "manifest.jsonl");
            long bytes = 0;
            bool present = File.Exists(manifest);
            if (present)
            {
                try
                {
                    bytes = new FileInfo(manifest).Length;
                }
                catch
                {
                }
            }
            rows.Add(new SessionRow
            {
                Name             = d.Name,
                Path             = d.FullName,
                LastWriteUtc     = d.LastWriteTimeUtc,
                ManifestBytes    = bytes,
                ManifestPresent  = present,
                IsActive         = !string.IsNullOrEmpty(activeManifestPath)
                                 && string.Equals(manifest, activeManifestPath, StringComparison.OrdinalIgnoreCase)
            });
        }
        rows.Sort((a, b) => b.LastWriteUtc.CompareTo(a.LastWriteUtc));
        return rows;
    }
}
