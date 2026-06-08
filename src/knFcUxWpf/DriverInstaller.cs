/*
 * DriverInstaller.cs
 * SCM-based install/uninstall + FilterLoad/Unload. Requires elevation.
 */

using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;
using Microsoft.Win32;

namespace KnFc.Ux;

public static class DriverInstaller
{
    public const string ServiceName  = "knFcFlt";
    public const string DisplayName  = "knFileCatcher Minifilter (internal/dev)";
    public const string ServiceGroup = "FSFilter Activity Monitor";
    public const string Altitude     = "999999.9";
    public const string InstanceName = "knFcFlt Instance";

    public static string SysSourcePath()
    {
        /* In a single-file self-contained publish, AppContext.BaseDirectory
         * points at the .NET runtime's self-extraction temp dir, NOT at
         * the folder the user dropped the EXE in. Environment.ProcessPath
         * always returns the real .exe path.
         */
        string? exe = Environment.ProcessPath;
        string dir = !string.IsNullOrEmpty(exe)
            ? Path.GetDirectoryName(exe)!
            : AppContext.BaseDirectory;
        return Path.Combine(dir, "knFcFlt.sys");
    }

    public static string SysDestPath()
    {
        var sb = new StringBuilder(260);
        Native.GetSystemDirectory(sb, (uint)sb.Capacity);
        return Path.Combine(sb.ToString(), "drivers", "knFcFlt.sys");
    }

    /// <summary>
    /// Returns 0 on success, Win32 error otherwise.
    /// installedHere = true if this call newly registered the service.
    /// </summary>
    private static bool IsSysStale(string src, string dst)
    {
        try
        {
            if (!File.Exists(dst))
            {
                return true;
            }
            var fiSrc = new FileInfo(src);
            var fiDst = new FileInfo(dst);
            if (fiSrc.Length != fiDst.Length)
            {
                return true;
            }
            /* Same length - fall back to content hash. The driver is ~35 KB
             * so this is cheap and avoids tripping on clock skew. */
            return !ContentHashEquals(src, dst);
        }
        catch
        {
            /* If we can't compare (locked, ACL, etc.), assume in-place is fine. */
            return false;
        }
    }

    private static bool ContentHashEquals(string a, string b)
    {
        using var sha = SHA256.Create();
        byte[] ha, hb;
        using (var fs = File.OpenRead(a)) { ha = sha.ComputeHash(fs); }
        using (var fs = File.OpenRead(b)) { hb = sha.ComputeHash(fs); }
        if (ha.Length != hb.Length)
        {
            return false;
        }
        for (int i = 0; i < ha.Length; ++i)
        {
            if (ha[i] != hb[i])
            {
                return false;
            }
        }
        return true;
    }

    /// <summary>
    /// Best-effort teardown of an existing knFcFlt registration so the
    /// caller can drop a new .sys onto disk and recreate the service.
    /// Returns true if the service slot is gone by the time we exit.
    /// </summary>
    private static bool ReinstallStaleDriver(IntPtr scm)
    {
        /* Step 1: yank any live filter instance. Safe to call when not
         * loaded - returns failure but no harm done. */
        try { Native.FilterUnload(ServiceName); } catch { }

        /* Step 2: stop and delete the SCM service entry. */
        IntPtr svc = Native.OpenService(scm, ServiceName,
            Native.SERVICE_ALL_ACCESS);
        if (svc != IntPtr.Zero)
        {
            try
            {
                Native.DeleteService(svc);
            }
            finally
            {
                Native.CloseServiceHandle(svc);
            }
        }

        /* Step 3: drop the on-disk .sys. The file may be marked
         * "delete on reboot" if still mapped - in that case the
         * subsequent File.Copy will fail and we'll fall back to the
         * old driver until next boot. */
        for (int i = 0; i < 5; ++i)
        {
            try
            {
                File.Delete(SysDestPath());
                break;
            }
            catch (IOException)
            {
                Thread.Sleep(200);
            }
        }

        /* Step 4: confirm the service is really gone before reporting OK. */
        IntPtr probe = Native.OpenService(scm, ServiceName, Native.SERVICE_QUERY_STATUS);
        if (probe != IntPtr.Zero)
        {
            Native.CloseServiceHandle(probe);
            return false;
        }
        return true;
    }

    public static int EnsureInstalled(out bool installedHere)
    {
        installedHere = false;

        IntPtr scm = Native.OpenSCManager(null, null, Native.SC_MANAGER_ALL_ACCESS);
        if (scm == IntPtr.Zero)
        {
            return Marshal.GetLastWin32Error();
        }
        try
        {
            IntPtr svc = Native.OpenService(scm, ServiceName, Native.SERVICE_QUERY_STATUS);
            if (svc != IntPtr.Zero)
            {
                Native.CloseServiceHandle(svc);
                /* Even on a re-run we want to make sure the cert is still
                 * in the trust stores - someone may have wiped them. */
                TrustDriverCert();

                /* Self-upgrade: if the .sys shipped next to this EXE differs
                 * from the one currently registered, drop the service so
                 * the install path below redeploys it. Without this, the
                 * UX would silently keep running an old driver across
                 * upgrades. */
                string srcPath = SysSourcePath();
                string dstPath = SysDestPath();
                if (File.Exists(srcPath) && IsSysStale(srcPath, dstPath))
                {
                    if (!ReinstallStaleDriver(scm))
                    {
                        /* Reinstall failed: keep the existing service so
                         * the user can at least run with the old driver. */
                        return 0;
                    }
                    /* fall through to the fresh-install path below */
                }
                else
                {
                    /* Re-stamp the altitude registry every run too.
                     * Some external cleanup tools wipe the Instances
                     * subkey; without this the filter would load but
                     * never attach. Cheap idempotent operation. */
                    WriteAltitudeRegistry();
                    return 0;
                }
            }
            else
            {
                int err = Marshal.GetLastWin32Error();
                if (err != Native.ERROR_SERVICE_DOES_NOT_EXIST)
                {
                    return err;
                }
            }

            string src = SysSourcePath();
            string dst = SysDestPath();
            if (!File.Exists(src))
            {
                return 2;  /* ERROR_FILE_NOT_FOUND */
            }

            /* Lift the self-signed dev cert out of the .sys and install it
             * into LocalMachine\Root + TrustedPublisher so the kernel
             * accepts the driver under testsigning. This is what saves
             * the user from a manual `Import-PfxCertificate` step. */
            TrustDriverCert();

            try
            {
                File.Copy(src, dst, overwrite: true);
            }
            catch (IOException)
            {
                /* Sharing violation: the in-use binary on disk is fine to leave */
            }
            catch (UnauthorizedAccessException)
            {
                return Native.ERROR_ACCESS_DENIED;
            }

            svc = Native.CreateService(
                scm, ServiceName, DisplayName,
                Native.SERVICE_ALL_ACCESS,
                Native.SERVICE_FILE_SYSTEM_DRIVER,
                Native.SERVICE_DEMAND_START,
                Native.SERVICE_ERROR_NORMAL,
                dst, ServiceGroup,
                IntPtr.Zero, null, null, null);
            if (svc == IntPtr.Zero)
            {
                return Marshal.GetLastWin32Error();
            }
            Native.CloseServiceHandle(svc);

            WriteAltitudeRegistry();
            installedHere = true;
            return 0;
        }
        finally
        {
            Native.CloseServiceHandle(scm);
        }
    }

    /* ---------- self-signed dev cert auto-trust ---------- */

    private const string CertSubject = "CN=knFcFlt-Dev";

    private static void TrustDriverCert()
    {
        string sysPath = SysSourcePath();
        if (!File.Exists(sysPath))
        {
            return;
        }
        try
        {
            /* X509Certificate2(filePath) only reads .cer/.pfx/.pem; for
             * a PE file with an embedded Authenticode signature we need
             * X509Certificate.CreateFromSignedFile.
             */
            X509Certificate raw = X509Certificate.CreateFromSignedFile(sysPath);
            using var cert = new X509Certificate2(raw);
            AddToStore(cert, StoreName.Root);
            AddToStore(cert, StoreName.TrustedPublisher);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine("TrustDriverCert: " + ex.Message);
            /* Best-effort: fall through and hope testsigning alone is enough */
        }
    }

    private static void AddToStore(X509Certificate2 cert, StoreName name)
    {
        try
        {
            using var store = new X509Store(name, StoreLocation.LocalMachine);
            store.Open(OpenFlags.ReadWrite);
            store.Add(cert);
        }
        catch
        {
            /* already present, or ACL denied (no admin) */
        }
    }

    private static void UntrustDriverCert()
    {
        foreach (StoreName name in new[] { StoreName.Root, StoreName.TrustedPublisher })
        {
            try
            {
                using var store = new X509Store(name, StoreLocation.LocalMachine);
                store.Open(OpenFlags.ReadWrite);
                var hits = store.Certificates.Find(
                    X509FindType.FindBySubjectDistinguishedName, CertSubject, false);
                foreach (var c in hits)
                {
                    try { store.Remove(c); } catch { }
                }
            }
            catch
            {
                /* best effort */
            }
        }
    }

    private static void WriteAltitudeRegistry()
    {
        using (var instances = Registry.LocalMachine.CreateSubKey(
            @"SYSTEM\CurrentControlSet\Services\knFcFlt\Instances"))
        {
            instances?.SetValue("DefaultInstance", InstanceName, RegistryValueKind.String);
        }
        using (var inst = Registry.LocalMachine.CreateSubKey(
            @"SYSTEM\CurrentControlSet\Services\knFcFlt\Instances\knFcFlt Instance"))
        {
            if (inst != null)
            {
                inst.SetValue("Altitude", Altitude, RegistryValueKind.String);
                inst.SetValue("Flags",    0,        RegistryValueKind.DWord);
            }
        }
    }

    public static void Uninstall()
    {
        IntPtr scm = Native.OpenSCManager(null, null, Native.SC_MANAGER_ALL_ACCESS);
        if (scm == IntPtr.Zero)
        {
            return;
        }
        try
        {
            IntPtr svc = Native.OpenService(scm, ServiceName, Native.SERVICE_DELETE);
            if (svc != IntPtr.Zero)
            {
                Native.DeleteService(svc);
                Native.CloseServiceHandle(svc);
            }
        }
        finally
        {
            Native.CloseServiceHandle(scm);
        }

        try
        {
            File.Delete(SysDestPath());
        }
        catch
        {
            /* may be in-use; OS will clean on reboot */
        }

        UntrustDriverCert();
    }

    /// <summary>
    /// Returns 0 on success. loadedHere = true only if this call moved
    /// the filter from unloaded to loaded.
    /// </summary>
    public static int LoadIfNeeded(out bool loadedHere)
    {
        loadedHere = false;

        /* FilterLoad fails with ERROR_PRIVILEGE_NOT_HELD (1314) on an
         * elevated token unless SeLoadDriverPrivilege is explicitly
         * enabled. UAC elevation alone is not enough.
         */
        EnableLoadDriverPrivilege();

        int hr = Native.FilterLoad(ServiceName);
        if (hr == 0)
        {
            loadedHere = true;
            return 0;
        }
        int code = Native.HResultToWin32(hr);
        if (code == Native.ERROR_ALREADY_EXISTS || code == Native.ERROR_SERVICE_ALREADY_RUNNING)
        {
            return 0;
        }
        return code;
    }

    private static void EnableLoadDriverPrivilege()
    {
        if (!Native.OpenProcessToken(
                Native.GetCurrentProcess(),
                Native.TOKEN_ADJUST_PRIVILEGES | Native.TOKEN_QUERY,
                out IntPtr token))
        {
            return;
        }
        try
        {
            if (!Native.LookupPrivilegeValue(null, Native.SE_LOAD_DRIVER_NAME, out Native.LUID luid))
            {
                return;
            }
            var tp = new Native.TOKEN_PRIVILEGES_ONE
            {
                PrivilegeCount = 1,
                Privilege0 = new Native.LUID_AND_ATTRIBUTES
                {
                    Luid       = luid,
                    Attributes = Native.SE_PRIVILEGE_ENABLED
                }
            };
            Native.AdjustTokenPrivileges(token, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
        }
        finally
        {
            Native.CloseHandle(token);
        }
    }

    public static void Unload()
    {
        Native.FilterUnload(ServiceName);
    }
}
