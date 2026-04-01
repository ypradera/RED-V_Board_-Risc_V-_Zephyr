using System;
using System.Windows;

namespace ShoeVisualizer;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        AppDomain.CurrentDomain.UnhandledException += (s, args) =>
        {
            MessageBox.Show($"Unhandled: {args.ExceptionObject}",
                "Error", MessageBoxButton.OK, MessageBoxImage.Error);
        };
        DispatcherUnhandledException += (s, args) =>
        {
            MessageBox.Show($"UI Error: {args.Exception}",
                "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            args.Handled = true;
        };
    }
}
