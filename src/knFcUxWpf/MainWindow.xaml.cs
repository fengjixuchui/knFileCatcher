/*
 * MainWindow.xaml.cs
 * Thin code-behind: Enter on the history grid + double-click both
 * launch OpenSelectedCommand via the ViewModel.
 */

using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace KnFc.Ux;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        PreviewKeyDown    += MainWindow_PreviewKeyDown;
        PreviewMouseWheel += MainWindow_PreviewMouseWheel;
    }

    /// <summary>
    /// Forward wheel events to the closest ScrollViewer that is actually
    /// scrollable in the requested direction. Walking from the element
    /// under the cursor up the visual tree lets us pick the DataGrid's
    /// inner SV when the cursor is over its rows, and fall through to
    /// the tab's outer SV when the cursor is over an empty area. Only
    /// mark the event handled when a real scroll occurred - otherwise
    /// we'd swallow the wheel and break the inner control's default
    /// behavior on un-scrollable cards.
    /// </summary>
    private static void MainWindow_PreviewMouseWheel(object sender, MouseWheelEventArgs e)
    {
        if (e.Handled)
        {
            return;
        }
        for (DependencyObject? d = e.OriginalSource as DependencyObject; d != null; )
        {
            if (d is ScrollViewer sv && sv.ScrollableHeight > 0)
            {
                double prev = sv.VerticalOffset;
                sv.ScrollToVerticalOffset(prev - e.Delta);
                if (sv.VerticalOffset != prev)
                {
                    e.Handled = true;
                    return;
                }
                /* This SV is scrollable in principle but already at the
                 * edge in our direction; keep walking up so a parent SV
                 * can pick up the wheel (chain-scroll behavior). */
            }
            DependencyObject? next = VisualTreeHelper.GetParent(d);
            if (next == null && d is FrameworkElement fe)
            {
                next = fe.Parent;
            }
            d = next;
        }
    }

    private void MainWindow_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter
            && DataContext is MainViewModel vm
            && vm.OpenSelectedCommand.CanExecute(null))
        {
            vm.OpenSelectedCommand.Execute(null);
            e.Handled = true;
        }
    }

    private void History_DoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (DataContext is MainViewModel vm
            && vm.OpenSelectedCommand.CanExecute(null))
        {
            vm.OpenSelectedCommand.Execute(null);
        }
    }

    private void Sessions_DoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (DataContext is MainViewModel vm
            && vm.OpenSessionRowCommand.CanExecute(null))
        {
            vm.OpenSessionRowCommand.Execute(null);
        }
    }
}
