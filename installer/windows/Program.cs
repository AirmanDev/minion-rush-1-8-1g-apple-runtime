using System.Windows;
using System.IO;

namespace MinionRushInstaller;

internal static class Program
{
    [STAThread]
    public static int Main()
    {
        try
        {
            var application = CreateApplication();
            var contract = new InstallerContract(Path.Combine(AppContext.BaseDirectory, "installer_ui.json"));
            return application.Run(new InstallerWindow(new InstallerState(contract)));
        }
        catch (Exception error)
        {
            MessageBox.Show(error.Message, "Installer unavailable", MessageBoxButton.OK, MessageBoxImage.Error);
            return 1;
        }
    }

    internal static Application CreateApplication()
    {
        var application = new Application();
        application.Resources.MergedDictionaries.Add(new ResourceDictionary
        {
            Source = new Uri("pack://application:,,,/PresentationFramework.Fluent;component/Themes/Fluent.xaml")
        });
        return application;
    }
}
