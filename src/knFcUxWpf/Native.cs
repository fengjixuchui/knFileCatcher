/*
 * Native.cs
 * P/Invoke surface used by DriverInstaller, DriverClient, BackupWorker.
 *
 * fltLib.dll      : FilterConnect / SendMessage / GetMessage / ReplyMessage
 *                   FilterLoad / FilterUnload
 * advapi32.dll    : SCM open/create/delete service
 * kernel32.dll    : CreateFile/Read/Write/Copy, QueryDosDevice, GetSystemDirectory
 */

using System;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace KnFc.Ux;

internal static class Native
{
    /* ---------- fltLib ---------- */

    [DllImport("fltLib.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    public static extern int FilterConnectCommunicationPort(
        [MarshalAs(UnmanagedType.LPWStr)] string lpPortName,
        uint dwOptions,
        IntPtr lpContext,
        ushort wSizeOfContext,
        IntPtr lpSecurityAttributes,
        out SafeFileHandle hPort);

    [DllImport("fltLib.dll", ExactSpelling = true)]
    public static extern int FilterSendMessage(
        SafeFileHandle hPort,
        IntPtr lpInBuffer,
        uint dwInBufferSize,
        IntPtr lpOutBuffer,
        uint dwOutBufferSize,
        out uint lpBytesReturned);

    [DllImport("fltLib.dll", ExactSpelling = true)]
    public static extern int FilterGetMessage(
        SafeFileHandle hPort,
        IntPtr lpMessageBuffer,
        uint dwMessageBufferSize,
        IntPtr lpOverlapped);


    [DllImport("fltLib.dll", ExactSpelling = true)]
    public static extern int FilterReplyMessage(
        SafeFileHandle hPort,
        IntPtr lpReplyBuffer,
        uint dwReplyBufferSize);

    [DllImport("fltLib.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    public static extern int FilterLoad([MarshalAs(UnmanagedType.LPWStr)] string lpFilterName);

    [DllImport("fltLib.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    public static extern int FilterUnload([MarshalAs(UnmanagedType.LPWStr)] string lpFilterName);

    /* ---------- advapi32 (SCM) ---------- */

    public const uint SC_MANAGER_ALL_ACCESS      = 0xF003F;
    public const uint SERVICE_ALL_ACCESS         = 0xF01FF;
    public const uint SERVICE_QUERY_STATUS       = 0x0004;
    public const uint SERVICE_DELETE             = 0x10000;
    public const uint SERVICE_FILE_SYSTEM_DRIVER = 0x00000002;
    public const uint SERVICE_DEMAND_START       = 0x00000003;
    public const uint SERVICE_ERROR_NORMAL       = 0x00000001;

    public const int  ERROR_SERVICE_DOES_NOT_EXIST   = 1060;
    public const int  ERROR_SERVICE_ALREADY_RUNNING  = 1056;
    public const int  ERROR_ALREADY_EXISTS           = 183;
    public const int  ERROR_OPERATION_ABORTED        = 995;
    public const int  ERROR_INVALID_HANDLE           = 6;
    public const int  ERROR_NO_MORE_ITEMS            = 259;
    public const int  ERROR_ACCESS_DENIED            = 5;
    public const int  ERROR_DISK_FULL                = 112;
    public const int  ERROR_PRIVILEGE_NOT_HELD       = 1314;

    /* ---- Token privilege management (SeLoadDriverPrivilege) ---- */

    public const uint TOKEN_QUERY              = 0x0008;
    public const uint TOKEN_ADJUST_PRIVILEGES   = 0x0020;
    public const uint SE_PRIVILEGE_ENABLED      = 0x0002;
    public const string SE_LOAD_DRIVER_NAME     = "SeLoadDriverPrivilege";

    [StructLayout(LayoutKind.Sequential)]
    public struct LUID
    {
        public uint LowPart;
        public int  HighPart;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct LUID_AND_ATTRIBUTES
    {
        public LUID Luid;
        public uint Attributes;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct TOKEN_PRIVILEGES_ONE
    {
        public uint PrivilegeCount;
        public LUID_AND_ATTRIBUTES Privilege0;
    }

    [DllImport("kernel32.dll")]
    public static extern IntPtr GetCurrentProcess();

    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern bool OpenProcessToken(
        IntPtr ProcessHandle, uint DesiredAccess, out IntPtr TokenHandle);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool LookupPrivilegeValue(
        string? lpSystemName, string lpName, out LUID lpLuid);

    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern bool AdjustTokenPrivileges(
        IntPtr TokenHandle,
        bool DisableAllPrivileges,
        ref TOKEN_PRIVILEGES_ONE NewState,
        uint BufferLength,
        IntPtr PreviousState,
        IntPtr ReturnLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr hObject);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr OpenSCManager(string? machineName, string? databaseName, uint dwAccess);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr OpenService(IntPtr hSCManager, string serviceName, uint dwAccess);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateService(
        IntPtr hSCManager,
        string serviceName,
        string displayName,
        uint dwDesiredAccess,
        uint dwServiceType,
        uint dwStartType,
        uint dwErrorControl,
        string binaryPathName,
        string? loadOrderGroup,
        IntPtr lpdwTagId,
        string? dependencies,
        string? serviceStartName,
        string? password);

    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern bool DeleteService(IntPtr hService);

    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern bool CloseServiceHandle(IntPtr hSCObject);

    /* ---------- kernel32 ---------- */

    public const uint GENERIC_READ              = 0x80000000;
    public const uint GENERIC_WRITE             = 0x40000000;
    public const uint FILE_SHARE_READ           = 0x1;
    public const uint FILE_SHARE_WRITE          = 0x2;
    public const uint FILE_SHARE_DELETE         = 0x4;
    public const uint CREATE_ALWAYS             = 2;
    public const uint OPEN_EXISTING             = 3;
    public const uint FILE_ATTRIBUTE_NORMAL     = 0x80;
    public const uint FILE_FLAG_SEQUENTIAL_SCAN = 0x08000000;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern SafeFileHandle CreateFile(
        string lpFileName,
        uint dwDesiredAccess,
        uint dwShareMode,
        IntPtr lpSecurityAttributes,
        uint dwCreationDisposition,
        uint dwFlagsAndAttributes,
        IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool ReadFile(
        SafeFileHandle hFile, IntPtr buffer, uint nNumberOfBytesToRead,
        out uint lpNumberOfBytesRead, IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool WriteFile(
        SafeFileHandle hFile, IntPtr buffer, uint nNumberOfBytesToWrite,
        out uint lpNumberOfBytesWritten, IntPtr lpOverlapped);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool CopyFile(string existingFile, string newFile, bool failIfExists);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    public static extern uint GetSystemDirectory(StringBuilder lpBuffer, uint uSize);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern uint QueryDosDevice(
        [MarshalAs(UnmanagedType.LPWStr)] string lpDeviceName,
        [MarshalAs(UnmanagedType.LPWStr)] StringBuilder lpTargetPath,
        uint ucchMax);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CancelIoEx(SafeFileHandle hFile, IntPtr lpOverlapped);

    public static int HResultToWin32(int hr)
    {
        /* SEVERITY/FACILITY/CODE: low 16 bits are the Win32 code when FACILITY=WIN32 */
        return hr & 0xFFFF;
    }
}
