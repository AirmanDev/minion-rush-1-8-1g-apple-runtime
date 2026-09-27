using System.Diagnostics;
using System.Reflection;
using System.Text;
using System.Text.Json;
using MinionRushInstaller;

internal static class InstallerWindowsTests
{
    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static JsonElement Event(string fields) => JsonDocument.Parse(
        "{\"protocol\":1," + fields + "}").RootElement.Clone();

    public static async Task<int> Main(string[] arguments)
    {
        if (arguments.Contains("--fake-backend")) return await FakeBackend(arguments);
        var contract = new InstallerContract(Path.Combine(AppContext.BaseDirectory, "installer_ui.json"));
        var state = new InstallerState(contract);
        Require(!state.CanInstall, "Initial installation must be disabled");
        state.Receive(Event("\"type\":\"result\",\"devices\":[{\"id\":\"one\",\"udid\":\"registered\",\"name\":\"iPhone\",\"os\":\"27.0\"}]"));
        Require(state.SelectedDevice == "one", "One device can be selected automatically");
        var ipa = JsonSerializer.Serialize(new
        {
            protocol = 1,
            type = "result",
            ipa = new
            {
                bundle = "org.example.game",
                team = "ABCDEFGHIJ",
                expires = DateTimeOffset.UtcNow.AddDays(1).ToString("O"),
                devices = new[] { "registered" },
                minimum_os = "17",
                bytes = 100
            }
        });
        state.Receive(JsonDocument.Parse(ipa).RootElement);
        state.CommitArchive("path with spaces.ipa");
        Require(state.CanInstall, "A device-bound, unexpired IPA enables installation");
        state.SelectedDevice = "missing";
        Require(!state.CanInstall, "Missing devices must not enable installation");
        state.SelectedDevice = "one";
        state.Begin("Installing");
        Require(!state.CanInstall, "Busy installation must be disabled");
        state.Cancel();
        state.Receive(Event("\"type\":\"progress\",\"message\":\"Still uploading\""));
        Require(state.Status == contract.Text("cancelling"), "Late progress must not overwrite cancellation");
        state.Finish();
        state.Receive(Event("\"type\":\"result\",\"devices\":[{\"id\":\"two\",\"udid\":\"other\",\"name\":\"iPad\",\"os\":\"27.0\"},{\"id\":\"three\",\"udid\":\"third\",\"name\":\"iPhone\",\"os\":\"27.0\"}]"));
        Require(state.SelectedDevice == "", "Multiple devices require a deliberate choice");
        state.SelectedDevice = "two";
        Require(!state.CanInstall, "A device outside the profile must be rejected");
        state.AppendLog(new string('a', 140000));
        Require(state.Log.Length <= 96 * 1024, "The rendered log tail must be bounded");

        var executable = Environment.ProcessPath!;
        var prefix = Path.GetFileNameWithoutExtension(executable).Equals("dotnet", StringComparison.OrdinalIgnoreCase)
            ? new[] { Assembly.GetExecutingAssembly().Location, "--fake-backend" }
            : new[] { "--fake-backend" };
        var backend = new BackendClient(executable, prefix);
        var events = new List<JsonElement>();
        await backend.Run(AppContext.BaseDirectory, ["echo", "path with spaces; not a command"],
            item => { events.Add(item); return Task.CompletedTask; }, CancellationToken.None);
        Require(events[0].GetProperty("message").GetString() == "path with spaces; not a command", "Argument boundaries");
        Require(events[1].GetProperty("message").GetString() == "\u0151\u0171", "UTF-8 process decoding");
        using var cancellation = new CancellationTokenSource();
        var watch = Stopwatch.StartNew();
        try
        {
            await backend.Run(AppContext.BaseDirectory, ["wait"], item =>
            {
                cancellation.Cancel(); return Task.CompletedTask;
            }, cancellation.Token);
            throw new InvalidOperationException("Cancellation was ignored");
        }
        catch (OperationCanceledException) { }
        Require(watch.Elapsed < TimeSpan.FromSeconds(10), "Graceful cancellation must not wait for forced termination");
        try
        {
            await backend.Run(AppContext.BaseDirectory, ["invalid"], _ => Task.CompletedTask, CancellationToken.None);
            throw new InvalidOperationException("An invalid protocol was accepted");
        }
        catch (InvalidDataException) { }
        Console.WriteLine("INSTALLER: Windows state and process tests passed");
        return 0;
    }

    private static async Task<int> FakeBackend(string[] arguments)
    {
        Console.OutputEncoding = new UTF8Encoding(false);
        if (arguments.Contains("invalid"))
        {
            Console.WriteLine("{\"protocol\":99,\"type\":\"result\"}"); return 0;
        }
        Console.WriteLine(JsonSerializer.Serialize(new { protocol = 1, type = "log", message = arguments[^1] }));
        if (arguments.Contains("wait"))
        {
            return await Console.In.ReadLineAsync() == "cancel" ? 130 : 1;
        }
        Console.Write("{\"protocol\":1,\"type\":\"log\",\"message\":\"");
        Console.Out.Flush();
        await Task.Delay(30);
        Console.WriteLine("\u0151\u0171\"}");
        Console.WriteLine("{\"protocol\":1,\"type\":\"result\",\"installed\":true}"); return 0;
    }
}
