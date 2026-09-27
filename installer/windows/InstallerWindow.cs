using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;

namespace MinionRushInstaller;

internal sealed class InstallerWindow : Window
{
    private readonly InstallerState state;
    private readonly string workspace = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MinionRushInstaller");
    private readonly Button archiveButton;
    private readonly ComboBox devicePicker;
    private readonly Button refreshButton;
    private readonly Button installButton;
    private readonly Button cancelButton;
    private readonly Button logButton;
    private readonly Button openLogButton;
    private readonly TextBlock assetSummary = Text("", 13);
    private readonly TextBlock signingSummary = Text("", 13);
    private readonly TextBlock status = Text("", 13);
    private readonly TextBox log = new()
    {
        IsReadOnly = true,
        AcceptsReturn = true,
        TextWrapping = TextWrapping.NoWrap,
        FontFamily = new FontFamily("Consolas"),
        FontSize = 12,
        VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
        Height = 180
    };
    private readonly CheckBox follow = new() { IsChecked = true };
    private readonly StackPanel logBody = new() { Visibility = Visibility.Collapsed };
    private CancellationTokenSource? operation;
    private bool closeAfterCancellation;

    public InstallerWindow(InstallerState state, bool discoverOnLaunch = true)
    {
        this.state = state;
        Title = state.Contract.Title;
        Width = 740; Height = 880; MinWidth = state.Contract.MinimumWidth; MinHeight = 660;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        SetResourceReference(BackgroundProperty, SystemColors.ControlBrushKey);
        DataContext = state;
        archiveButton = Button(state.Contract.Text("choose"), ChooseArchive);
        archiveButton.MinHeight = 100;
        archiveButton.HorizontalContentAlignment = HorizontalAlignment.Stretch;
        archiveButton.AllowDrop = true;
        archiveButton.DragOver += ArchiveDragOver;
        archiveButton.Drop += ArchiveDrop;
        AutomationProperties.SetAutomationId(archiveButton, "installer.chooseArchive");
        devicePicker = new ComboBox
        {
            MinHeight = 44,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            DisplayMemberPath = nameof(Device.Display),
            SelectedValuePath = nameof(Device.Id),
            IsEditable = false
        };
        devicePicker.SetBinding(ItemsControl.ItemsSourceProperty, new Binding(nameof(InstallerState.Devices)));
        devicePicker.SetBinding(ComboBox.SelectedValueProperty, new Binding(nameof(InstallerState.SelectedDevice))
        { Mode = BindingMode.TwoWay, UpdateSourceTrigger = UpdateSourceTrigger.PropertyChanged });
        AutomationProperties.SetName(devicePicker, state.Contract.Text("device"));
        AutomationProperties.SetAutomationId(devicePicker, "installer.device");
        refreshButton = Button(state.Contract.Text("refresh"), () => Run(["devices"], state.Contract.Text("checking")));
        installButton = Button(state.Contract.Text("install"), () =>
        {
            if (state.CanInstall) Run(["install", state.ArchivePath!, "--device", state.SelectedDevice], state.Contract.Text("working"));
        });
        installButton.IsDefault = true;
        AutomationProperties.SetAutomationId(installButton, "installer.install");
        cancelButton = Button(state.Contract.Text("cancel"), Cancel);
        logButton = Button(state.Contract.Text("logs"), () =>
        {
            if (logBody.Visibility == Visibility.Visible)
            {
                logBody.Visibility = Visibility.Collapsed; UpdateDisclosure();
            }
            else ShowLog();
        });
        logButton.HorizontalContentAlignment = HorizontalAlignment.Left;
        AutomationProperties.SetAutomationId(logButton, "installer.logDisclosure");
        openLogButton = Button(state.Contract.Text("openLog"), () =>
        {
            if (state.LogPath is { } path) Open(path);
        });
        follow.Content = state.Contract.Text("followLog");
        follow.Checked += (_, _) => log.ScrollToEnd();
        Content = BuildLayout();
        state.PropertyChanged += (_, _) => UpdateState();
        Closing += OnClosing;
        if (discoverOnLaunch) Loaded += (_, _) => Run(["devices"], state.Contract.Text("checking"));
        InputBindings.Add(new KeyBinding(new ActionCommand(ChooseArchive, () => !state.Busy), Key.O, ModifierKeys.Control));
        UpdateState();
    }

    private UIElement BuildLayout()
    {
        var root = new DockPanel();
        var footer = new Grid { Margin = new Thickness(20), MinHeight = 44 };
        footer.ColumnDefinitions.Add(new ColumnDefinition());
        footer.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        status.VerticalAlignment = VerticalAlignment.Center;
        status.Margin = new Thickness(0);
        AutomationProperties.SetAutomationId(status, "installer.status");
        footer.Children.Add(status);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(16, 0, 0, 0) };
        actions.Children.Add(cancelButton);
        installButton.Margin = new Thickness(12, 0, 0, 0);
        actions.Children.Add(installButton);
        Grid.SetColumn(actions, 1); footer.Children.Add(actions);
        DockPanel.SetDock(footer, Dock.Bottom); root.Children.Add(footer);
        var content = new StackPanel { Margin = new Thickness(24) };
        var header = new Grid { Margin = new Thickness(0, 0, 0, 18) };
        header.ColumnDefinitions.Add(new ColumnDefinition());
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var heading = new StackPanel();
        heading.Children.Add(Text(state.Contract.Title, 28, FontWeights.SemiBold));
        heading.Children.Add(Text(state.Contract.Subtitle, 13));
        header.Children.Add(heading);
        var files = Button(state.Contract.Text("workspace"), () => { Directory.CreateDirectory(workspace); Open(workspace); });
        Grid.SetColumn(files, 1); header.Children.Add(files); content.Children.Add(header);
        foreach (var section in state.Contract.Sections)
        {
            var body = new StackPanel();
            body.Children.Add(Text(section.Title, 15, FontWeights.SemiBold));
            switch (section.Id)
            {
                case "release":
                    body.Children.Add(archiveButton); body.Children.Add(assetSummary);
                    body.Children.Add(Text(state.Contract.Text("releaseHelp"), 12));
                    break;
                case "device":
                    body.Children.Add(Text(state.Contract.Text("device"), 13));
                    var selection = new Grid();
                    selection.ColumnDefinitions.Add(new ColumnDefinition());
                    selection.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                    selection.Children.Add(devicePicker);
                    refreshButton.Margin = new Thickness(12, 0, 0, 0);
                    Grid.SetColumn(refreshButton, 1); selection.Children.Add(refreshButton);
                    body.Children.Add(selection);
                    body.Children.Add(Text(state.Contract.Text("deviceHelp"), 12));
                    break;
                case "signing":
                    body.Children.Add(signingSummary);
                    body.Children.Add(Text(state.Contract.Text("signingHelp"), 12));
                    body.Children.Add(Text(state.Contract.Text("personalTeam"), 12));
                    body.Children.Add(Button(state.Contract.Text("accountHelp"), () =>
                        Open("https://developer.apple.com/help/account/basics/about-your-developer-account")));
                    break;
                default: throw new InvalidDataException("Unsupported installer section.");
            }
            content.Children.Add(Card(body));
        }
        var logActions = new DockPanel { Margin = new Thickness(0, 0, 0, 12) };
        follow.HorizontalAlignment = HorizontalAlignment.Right;
        DockPanel.SetDock(follow, Dock.Right); logActions.Children.Add(follow);
        logActions.Children.Add(openLogButton); logBody.Children.Add(logActions); logBody.Children.Add(log);
        var activity = new StackPanel(); activity.Children.Add(logButton); activity.Children.Add(logBody);
        content.Children.Add(Card(activity));
        root.Children.Add(new ScrollViewer
        {
            Content = content,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled
        });
        return root;
    }

    private void UpdateState()
    {
        archiveButton.IsEnabled = !state.Busy;
        archiveButton.Content = new StackPanel
        {
            Children =
            {
                Text(state.Contract.Text("drop"), 15, FontWeights.SemiBold),
                Text(state.ArchivePath is null ? state.Contract.Text("choose") : Path.GetFileName(state.ArchivePath), 13)
            }
        };
        assetSummary.Text = state.Ipa is null ? state.Contract.Text("noAssets")
            : $"{state.Contract.Text("assetsReady")} - {state.Ipa.Bytes / 1024d / 1024:F1} MB";
        signingSummary.Text = state.Ipa is null ? state.Contract.Text("signingNeeded")
            : $"{state.Contract.Text("team")}: {state.Ipa.Team}\n{state.Contract.Text("bundle")}: {state.Ipa.Bundle}\n{state.Contract.Text("expires")}: {state.Ipa.Expires}";
        devicePicker.IsEnabled = !state.Busy && state.Devices.Count > 0;
        refreshButton.IsEnabled = !state.Busy;
        installButton.IsEnabled = state.CanInstall;
        cancelButton.Visibility = state.Busy ? Visibility.Visible : Visibility.Collapsed;
        cancelButton.IsEnabled = state.Busy && !state.Cancelling;
        status.Text = state.Status;
        if (state.Error is null) status.SetResourceReference(TextBlock.ForegroundProperty, SystemColors.ControlTextBrushKey);
        else status.Foreground = Brushes.Firebrick;
        openLogButton.IsEnabled = state.LogPath is not null;
        UpdateDisclosure();
        if (log.Text != state.Log)
        {
            log.Text = state.Log;
            if (follow.IsChecked == true) log.ScrollToEnd();
        }
    }

    private async void Run(string[] arguments, string phase, string? archive = null)
    {
        if (state.Busy) return;
        using var cancellation = new CancellationTokenSource();
        operation = cancellation;
        if (arguments[0] != "devices") ShowLog();
        state.Begin(phase);
        try
        {
            await new BackendClient().Run(workspace, arguments, async item =>
                await Dispatcher.InvokeAsync(() => state.Receive(item)), cancellation.Token);
            if (state.Error is null && archive is not null) state.CommitArchive(archive);
        }
        catch (OperationCanceledException) { state.Fail(state.Contract.Text("cancelled")); }
        catch (Exception error)
        {
            if (state.Error is null) state.Fail(error.Message);
            ShowLog();
        }
        finally
        {
            operation = null;
            state.Finish();
            if (closeAfterCancellation) Close();
        }
    }

    private void ChooseArchive()
    {
        if (state.Busy) return;
        var chooser = new OpenFileDialog { Filter = "Signed iOS application (*.ipa)|*.ipa", Multiselect = false };
        if (chooser.ShowDialog(this) == true) Inspect(chooser.FileName);
    }

    private void Inspect(string path) => Run(["inspect", path], state.Contract.Text("working"), path);

    private void ArchiveDragOver(object sender, DragEventArgs args)
    {
        args.Effects = !state.Busy && IsArchiveDrop(args, out _) ? DragDropEffects.Copy : DragDropEffects.None;
        args.Handled = true;
    }

    private void ArchiveDrop(object sender, DragEventArgs args)
    {
        args.Handled = true;
        if (state.Busy) return;
        if (IsArchiveDrop(args, out var path)) Inspect(path!);
        else { state.Fail(state.Contract.Text("invalidDrop")); ShowLog(); }
    }

    private static bool IsArchiveDrop(DragEventArgs args, out string? path)
    {
        path = null;
        if (args.Data.GetData(DataFormats.FileDrop) is not string[] { Length: 1 } files
            || !string.Equals(Path.GetExtension(files[0]), ".ipa", StringComparison.OrdinalIgnoreCase)) return false;
        path = files[0]; return true;
    }

    private void Cancel()
    {
        state.Cancel();
        operation?.Cancel();
    }

    private void OnClosing(object? sender, CancelEventArgs args)
    {
        if (!state.Busy) return;
        args.Cancel = true;
        if (closeAfterCancellation) return;
        if (MessageBox.Show(this, state.Contract.Text("quitHelp"), state.Contract.Text("quitTitle"),
            MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No) == MessageBoxResult.Yes)
        {
            closeAfterCancellation = true; Cancel();
        }
    }

    private void UpdateDisclosure()
    {
        logButton.Content = state.Contract.Text("logs") + " - "
            + state.Contract.Text(logBody.Visibility == Visibility.Visible ? "hide" : "show");
        AutomationProperties.SetHelpText(logButton, state.Contract.Text(
            logBody.Visibility == Visibility.Visible ? "expanded" : "collapsed"));
    }

    private void ShowLog()
    {
        logBody.Visibility = Visibility.Visible;
        UpdateDisclosure();
        Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() => logBody.BringIntoView()));
    }

    private Button Button(string title, Action action)
    {
        var button = new Button
        {
            Content = title,
            MinHeight = 36,
            MinWidth = 80,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            Padding = new Thickness(12, 6, 12, 6)
        };
        AutomationProperties.SetName(button, title);
        button.Click += (_, _) =>
        {
            try { action(); }
            catch (Exception error)
            {
                state.Fail(error.Message); ShowLog();
            }
        };
        return button;
    }

    private static TextBlock Text(string value, double size, FontWeight? weight = null) => new()
    {
        Text = value,
        FontSize = size,
        FontWeight = weight ?? FontWeights.Normal,
        TextWrapping = TextWrapping.Wrap,
        Margin = new Thickness(0, 0, 0, 12)
    };

    private static Border Card(UIElement content)
    {
        var border = new Border
        {
            Child = content,
            Padding = new Thickness(20),
            CornerRadius = new CornerRadius(16),
            Margin = new Thickness(0, 0, 0, 18)
        };
        border.SetResourceReference(Border.BackgroundProperty, SystemColors.WindowBrushKey); return border;
    }

    private static void Open(string path) => Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
}

internal sealed class ActionCommand(Action execute, Func<bool> canExecute) : ICommand
{
    public event EventHandler? CanExecuteChanged
    {
        add => CommandManager.RequerySuggested += value;
        remove => CommandManager.RequerySuggested -= value;
    }
    public bool CanExecute(object? parameter) => canExecute();
    public void Execute(object? parameter) => execute();
}
