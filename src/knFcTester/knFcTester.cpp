/*
 * knFcTester.cpp
 * Small console harness for stressing the knFileCatcher capture path.
 *
 * Parent mode (no arg):
 *   1. Copies its own EXE into %TEMP% as five unique names:
 *      knFcTester-<pid>-<i>.exe
 *   2. Spawns each copy as a child with the --child argument.
 *   3. Waits for all children to finish, then deletes the copies.
 *
 * Child mode (--child):
 *   1. Asks Windows for a random temp name in %TEMP% (GetTempFileNameW).
 *   2. Writes the bytes "helloworld" into it.
 *   3. Deletes the file.
 *   4. Exits.
 *
 * Two interleaved short-lived bursts (5 parent copies, each child does
 * one create+write+delete) give the driver's queue + push paths a small
 * but deterministic load to chew on.
 *
 * Build:   src\knFcTester\build.ps1
 * Output:  build\tester\knFcTester.exe
 */

#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cwchar>

static constexpr int KnFcTesterIterations = 5;

static int
ChildMain()
{
    wchar_t tempDir[MAX_PATH];
    wchar_t fileName[MAX_PATH];
    HANDLE  h = INVALID_HANDLE_VALUE;
    DWORD   written = 0;
    constexpr char msg[] = "helloworld";

    if (GetTempPathW(MAX_PATH, tempDir) == 0)
    {
        fwprintf(stderr, L"child: GetTempPath failed %lu\n", GetLastError());
        return 1;
    }
    if (GetTempFileNameW(tempDir, L"knfc", 0, fileName) == 0)
    {
        fwprintf(stderr, L"child: GetTempFileName failed %lu\n", GetLastError());
        return 1;
    }

    h = CreateFileW(
        fileName,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        fwprintf(stderr, L"child: CreateFile failed %lu\n", GetLastError());
        return 1;
    }

    if (!WriteFile(h, msg, (DWORD)(sizeof(msg) - 1), &written, NULL)
        || written != (DWORD)(sizeof(msg) - 1))
    {
        fwprintf(stderr, L"child: WriteFile failed %lu\n", GetLastError());
        CloseHandle(h);
        DeleteFileW(fileName);
        return 1;
    }
    CloseHandle(h);

    if (!DeleteFileW(fileName))
    {
        fwprintf(stderr, L"child: DeleteFile failed %lu (path=%ls)\n",
            GetLastError(), fileName);
        return 1;
    }

    wprintf(L"child: pid=%lu file=%ls -> wrote %lu, deleted\n",
        GetCurrentProcessId(), fileName, written);
    return 0;
}

int
wmain(int argc, wchar_t** argv)
{
    wchar_t selfPath[MAX_PATH];
    wchar_t tempDir[MAX_PATH];
    wchar_t copyPaths[KnFcTesterIterations][MAX_PATH];
    HANDLE  procs[KnFcTesterIterations];
    int     spawned = 0;
    int     i;
    DWORD   pid = GetCurrentProcessId();

    if (argc > 1 && wcscmp(argv[1], L"--child") == 0)
    {
        return ChildMain();
    }

    for (i = 0; i < KnFcTesterIterations; ++i)
    {
        procs[i] = nullptr;
        copyPaths[i][0] = L'\0';
    }

    if (GetModuleFileNameW(nullptr, selfPath, MAX_PATH) == 0)
    {
        fwprintf(stderr, L"parent: GetModuleFileName failed %lu\n", GetLastError());
        return 1;
    }
    if (GetTempPathW(MAX_PATH, tempDir) == 0)
    {
        fwprintf(stderr, L"parent: GetTempPath failed %lu\n", GetLastError());
        return 1;
    }

    wprintf(L"parent: pid=%lu self=%ls\n", pid, selfPath);
    wprintf(L"parent: spawning %d children in %ls...\n", KnFcTesterIterations, tempDir);

    for (i = 0; i < KnFcTesterIterations; ++i)
    {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        wchar_t cmdLine[MAX_PATH + 32];

        _snwprintf(copyPaths[i], MAX_PATH,
            L"%sknFcTester-%lu-%d.exe", tempDir, pid, i);
        copyPaths[i][MAX_PATH - 1] = L'\0';

        if (!CopyFileW(selfPath, copyPaths[i], FALSE))
        {
            fwprintf(stderr, L"parent[%d]: CopyFile failed %lu (dst=%ls)\n",
                i, GetLastError(), copyPaths[i]);
            copyPaths[i][0] = L'\0';
            continue;
        }

        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

        _snwprintf(cmdLine, MAX_PATH + 32, L"\"%s\" --child", copyPaths[i]);
        cmdLine[MAX_PATH + 31] = L'\0';

        if (!CreateProcessW(
            nullptr,
            cmdLine,
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &si,
            &pi))
        {
            fwprintf(stderr, L"parent[%d]: CreateProcess failed %lu\n",
                i, GetLastError());
            DeleteFileW(copyPaths[i]);
            copyPaths[i][0] = L'\0';
            continue;
        }

        CloseHandle(pi.hThread);
        procs[spawned++] = pi.hProcess;
        wprintf(L"parent[%d]: spawned pid=%lu\n", i, pi.dwProcessId);
    }

    if (spawned > 0)
    {
        DWORD wait = WaitForMultipleObjects((DWORD)spawned, procs, TRUE, 30000);
        if (wait == WAIT_TIMEOUT)
        {
            fwprintf(stderr, L"parent: timeout waiting for children\n");
        }
        for (i = 0; i < spawned; ++i)
        {
            DWORD exitCode = 0;
            if (procs[i] != nullptr)
            {
                GetExitCodeProcess(procs[i], &exitCode);
                wprintf(L"parent: child[%d] exit=%lu\n", i, exitCode);
                CloseHandle(procs[i]);
            }
        }
    }

    /* Cleanup. EXE may still be locked momentarily after exit; retry. */
    for (i = 0; i < KnFcTesterIterations; ++i)
    {
        int retry;
        if (copyPaths[i][0] == L'\0')
        {
            continue;
        }
        for (retry = 0; retry < 10; ++retry)
        {
            if (DeleteFileW(copyPaths[i]))
            {
                break;
            }
            Sleep(50);
        }
    }

    wprintf(L"parent: done, %d children completed\n", spawned);
    return 0;
}
