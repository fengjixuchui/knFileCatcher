/*
 * App.xaml.cs
 * Single-instance gate + driver bootstrap + teardown.
 */

using System;
using System.Threading;
using System.Windows;

namespace KnFc.Ux;

public partial class App : Application
{
    private const string MutexName = "Global\\knFcUx_SingleInstance";

    private Mutex?         _instanceMutex;
    private bool           _ownsMutex;
    private MainViewModel? _vm;

    protected override void OnStartup(StartupEventArgs e)
    {
        _instanceMutex = new Mutex(initiallyOwned: true, name: MutexName, createdNew: out _ownsMutex);
        if (!_ownsMutex)
        {
            MessageBox.Show(
                "knFileCatcher is already running.",
                "knFileCatcher",
                MessageBoxButton.OK,
                MessageBoxImage.Information);
            Shutdown(1);
            return;
        }

        base.OnStartup(e);

        /* WPF's DoStartup (which is what would normally process StartupUri)
         * runs AFTER this override returns, so deferring to it left
         * MainWindow null at bootstrap time. Create the window here
         * explicitly, run Bootstrap synchronously, then Show it.
         */
        var window = new MainWindow();
        MainWindow  = window;

        if (window.DataContext is MainViewModel vm)
        {
            _vm = vm;
            if (!vm.Bootstrap(out string err))
            {
                MessageBox.Show(
                    "knFileCatcher bootstrap failed:\n\n" + err,
                    "knFileCatcher",
                    MessageBoxButton.OK,
                    MessageBoxImage.Error);
                Shutdown(2);
                return;
            }
        }
        else
        {
            MessageBox.Show(
                "knFileCatcher internal error: MainWindow.DataContext is not MainViewModel.\n"
                + "Type: " + (window.DataContext?.GetType().FullName ?? "<null>"),
                "knFileCatcher",
                MessageBoxButton.OK,
                MessageBoxImage.Error);
            Shutdown(3);
            return;
        }

        window.Show();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        try
        {
            _vm?.Shutdown();
        }
        catch
        {
            /* best-effort */
        }

        if (_ownsMutex && _instanceMutex != null)
        {
            try { _instanceMutex.ReleaseMutex(); } catch { }
        }
        _instanceMutex?.Dispose();
        _instanceMutex = null;
        base.OnExit(e);
    }
}
