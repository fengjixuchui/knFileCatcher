/*
 * BoolToVisibilityConverter.cs
 * Visibility = Visible when bound bool is true, Collapsed otherwise.
 */

using System;
using System.Globalization;
using System.Windows;
using System.Windows.Data;

namespace KnFc.Ux;

public sealed class BoolToVisibilityConverter : IValueConverter
{
    public static readonly BoolToVisibilityConverter Instance = new();

    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        bool b = value is bool x && x;
        return b ? Visibility.Visible : Visibility.Collapsed;
    }

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotSupportedException();
}
