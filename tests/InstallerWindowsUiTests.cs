using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Automation.Peers;
using System.Windows.Automation.Provider;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using MinionRushInstaller;

internal static class InstallerWindowsUiTests
{
    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static IEnumerable<T> Controls<T>(DependencyObject parent) where T : DependencyObject
    {
        if (parent is T item) yield return item;
        for (int index = 0; index < VisualTreeHelper.GetChildrenCount(parent); index++)
            foreach (var child in Controls<T>(VisualTreeHelper.GetChild(parent, index))) yield return child;
    }

    private static void Pump()
    {
        var frame = new DispatcherFrame();
        Dispatcher.CurrentDispatcher.BeginInvoke(DispatcherPriority.Background, new Action(() => frame.Continue = false));
        Dispatcher.PushFrame(frame);
    }

    [STAThread]
    public static int Main(string[] arguments)
    {
        _ = Program.CreateApplication();
        var contract = new InstallerContract(Path.Combine(AppContext.BaseDirectory, "installer_ui.json"));
        var state = new InstallerState(contract);
        state.Receive(JsonDocument.Parse("""
            {"protocol":1,"type":"result","devices":[
              {"id":"test","udid":"registered","name":"Test iPhone","os":"27.0"}]}
            """).RootElement);
        var window = new InstallerWindow(state, discoverOnLaunch: false)
        { Left = -10000, Top = -10000 };
        window.Show(); Pump(); window.UpdateLayout();
        try
        {
            var device = Controls<ComboBox>(window).Single();
            Require(device.ActualWidth > 350 && device.ActualHeight >= 44, "Device selector must span its row");
            var peer = new ComboBoxAutomationPeer(device);
            var expansion = (IExpandCollapseProvider)peer.GetPattern(PatternInterface.ExpandCollapse);
            expansion.Expand(); Pump();
            Require(device.IsDropDownOpen, "Native accessible selector must open");
            expansion.Collapse(); Pump();
            Require(!device.IsDropDownOpen, "Native accessible selector must close");
            var buttons = Controls<Button>(window).ToArray();
            var archive = buttons.Single(button => AutomationProperties.GetAutomationId(button) == "installer.chooseArchive");
            Require(archive.ActualWidth > 500 && archive.ActualHeight >= 100, "The full archive area must be a button");
            var install = buttons.Single(button => AutomationProperties.GetAutomationId(button) == "installer.install");
            Require(!install.IsEnabled, "Missing IPA must disable installation");
            var disclosure = buttons.Single(button => AutomationProperties.GetAutomationId(button) == "installer.logDisclosure");
            var invoke = (IInvokeProvider)new ButtonAutomationPeer(disclosure).GetPattern(PatternInterface.Invoke);
            invoke.Invoke(); Pump();
            Require(AutomationProperties.GetHelpText(disclosure) == contract.Text("expanded"), "The full log header must expand");
            invoke.Invoke(); Pump();
            Require(AutomationProperties.GetHelpText(disclosure) == contract.Text("collapsed"), "The log header must collapse");
            if (arguments.Length == 1)
            {
                Directory.CreateDirectory(arguments[0]);
                var content = (FrameworkElement)window.Content;
                var bitmap = new RenderTargetBitmap((int)content.ActualWidth, (int)content.ActualHeight, 96, 96, PixelFormats.Pbgra32);
                bitmap.Render(content);
                var encoder = new PngBitmapEncoder();
                encoder.Frames.Add(BitmapFrame.Create(bitmap));
                using var output = File.Create(Path.Combine(arguments[0], "windows-native.png"));
                encoder.Save(output);
            }
            Console.WriteLine("INSTALLER: Windows native UI tests passed");
            return 0;
        }
        finally { window.Close(); }
    }
}
