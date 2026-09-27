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

        void Capture(string name)
        {
            if (arguments.Length != 1) return;
            Directory.CreateDirectory(arguments[0]);
            var content = (FrameworkElement)window.Content;
            var rectangle = new Rect(0, 0, content.ActualWidth, content.ActualHeight);
            var surface = new DrawingVisual();
            using (var drawing = surface.RenderOpen())
            {
                drawing.DrawRectangle(window.Background, null, rectangle);
            }
            var bitmap = new RenderTargetBitmap((int)content.ActualWidth, (int)content.ActualHeight,
                96, 96, PixelFormats.Pbgra32);
            bitmap.Render(surface);
            bitmap.Render(content);
            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(bitmap));
            using var output = File.Create(Path.Combine(arguments[0], name + ".png"));
            encoder.Save(output);
        }

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
            Require(archive.ActualWidth > 500 && archive.ActualHeight >= 100,
                $"The full archive area must be a button (actual: {archive.ActualWidth} x {archive.ActualHeight})");
            var install = buttons.Single(button => AutomationProperties.GetAutomationId(button) == "installer.install");
            Require(!install.IsEnabled, "Missing IPA must disable installation");
            var disclosure = buttons.Single(button => AutomationProperties.GetAutomationId(button) == "installer.logDisclosure");
            Require(disclosure.ActualWidth > 500, "The log header must span its row");
            var scroll = Controls<ScrollViewer>(window).First();
            var invoke = (IInvokeProvider)new ButtonAutomationPeer(disclosure).GetPattern(PatternInterface.Invoke);
            invoke.Invoke(); Pump();
            Require(AutomationProperties.GetHelpText(disclosure) == contract.Text("expanded"), "The full log header must expand");
            Require(scroll.VerticalOffset > 0, "Expanded activity must scroll into view");
            invoke.Invoke(); Pump();
            Require(AutomationProperties.GetHelpText(disclosure) == contract.Text("collapsed"), "The log header must collapse");
            state.Receive(JsonSerializer.SerializeToElement(new
            {
                protocol = 1,
                type = "result",
                ipa = new
                {
                    bundle = "org.example.minionrush",
                    team = "ABCDEFGHIJ",
                    expires = DateTimeOffset.UtcNow.AddDays(1).ToString("O"),
                    devices = new[] { "registered" },
                    minimum_os = "17.0",
                    bytes = 749900000
                }
            }));
            state.CommitArchive("MinionRush.ipa"); Pump();
            scroll.ScrollToTop(); Pump();
            Require(install.IsEnabled, "A valid IPA and matching device must enable the native Install button");
            Capture("windows-native");
            window.Width = 640; window.Height = 660; Pump(); window.UpdateLayout();
            var root = (FrameworkElement)window.Content;
            var installBottom = install.TransformToAncestor(root).Transform(new Point(0, install.ActualHeight));
            Require(installBottom.Y <= root.ActualHeight, "Install must stay visible in the minimum-size window");
            Capture("windows-minimum");
            state.Fail("The selected device is unavailable. Unlock it, check the USB connection and Apple Mobile Device Service, then choose Refresh.");
            Pump();
            Require(!install.IsEnabled, "A failure must disable the native Install button");
            Capture("windows-failure");
            window.Width = 740; window.Height = 880;
            state.Begin("Uploading IPA: 50%");
            state.AppendLog("Inspecting signed IPA\nProfile and device matched\nUploading IPA: 50%");
            invoke.Invoke(); Pump();
            Require(!install.IsEnabled && !device.IsEnabled, "Busy native actions must be disabled");
            Capture("windows-working");
            Console.WriteLine("INSTALLER: Windows native UI tests passed");
            return 0;
        }
        finally { state.Finish(); window.Close(); }
    }
}
