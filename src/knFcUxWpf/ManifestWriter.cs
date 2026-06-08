/*
 * ManifestWriter.cs
 * Mutex-guarded append to <session>\manifest.jsonl, UTF-8 JSON lines.
 */

using System;
using System.IO;
using System.Text;
using System.Threading;

namespace KnFc.Ux;

public sealed class ManifestWriter : IDisposable
{
    private readonly object _lock = new();
    private FileStream? _fs;

    public string Path { get; private set; } = "";

    public bool Open(string path)
    {
        Path = path;
        try
        {
            _fs = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.Read,
                4096, FileOptions.WriteThrough);
            return true;
        }
        catch
        {
            _fs = null;
            return false;
        }
    }

    public void WriteEntry(
        ulong id, DateTime tsUtc, ulong pid, ulong rootPid, uint flags, ulong sizeHint,
        string srcNt, string dest, int status)
    {
        var sb = new StringBuilder(512);
        sb.Append('{');
        sb.Append("\"id\":").Append(id);
        sb.Append(",\"ts\":\"").Append(tsUtc.ToString("yyyy-MM-ddTHH:mm:ss.fffZ")).Append('"');
        sb.Append(",\"pid\":").Append(pid);
        sb.Append(",\"root_pid\":").Append(rootPid);
        sb.Append(",\"flags\":").Append(flags);
        sb.Append(",\"size_hint\":").Append(sizeHint);
        sb.Append(",\"src\":\"").Append(JsonEscape(srcNt)).Append('"');
        sb.Append(",\"dest\":\"").Append(JsonEscape(dest)).Append('"');
        sb.Append(",\"status\":").Append(status);
        sb.Append('}');
        sb.Append('\n');

        byte[] bytes = Encoding.UTF8.GetBytes(sb.ToString());
        lock (_lock)
        {
            if (_fs == null) { return; }
            _fs.Write(bytes, 0, bytes.Length);
            _fs.Flush();
        }
    }

    public void Dispose()
    {
        lock (_lock)
        {
            _fs?.Dispose();
            _fs = null;
        }
    }

    private static string JsonEscape(string s)
    {
        var sb = new StringBuilder(s.Length + 8);
        foreach (char c in s)
        {
            switch (c)
            {
            case '\\': sb.Append("\\\\"); break;
            case '"':  sb.Append("\\\""); break;
            case '\n': sb.Append("\\n");  break;
            case '\r': sb.Append("\\r");  break;
            case '\t': sb.Append("\\t");  break;
            default:
                if (c < 0x20)
                {
                    sb.Append("\\u");
                    sb.Append(((int)c).ToString("x4"));
                }
                else
                {
                    sb.Append(c);
                }
                break;
            }
        }
        return sb.ToString();
    }
}
