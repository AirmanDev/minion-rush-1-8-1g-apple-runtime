using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Text.Json;

namespace MinionRushInstaller;

internal sealed record Device(string Id, string Udid, string Name, string Os)
{
    public string Display => $"{Name} - {Os}";
}

internal sealed record IpaReport(string Bundle, string Team, string Expires,
    string[] Devices, string MinimumOs, long Bytes);

internal sealed class InstallerContract
{
    private readonly JsonElement content;
    public InstallerContract(string path) => content = JsonDocument.Parse(File.ReadAllText(path)).RootElement.Clone();
    public string Title => content.GetProperty("title").GetString()!;
    public string Subtitle => content.GetProperty("subtitle").GetString()!;
    public double MinimumWidth => content.GetProperty("minimumWidth").GetDouble();
    public string Text(string key)
    {
        if (content.GetProperty("windows").GetProperty("labels").TryGetProperty(key, out var value))
            return value.GetString()!;
        return content.GetProperty("labels").GetProperty(key).GetString()!;
    }
    public IEnumerable<(string Id, string Title)> Sections =>
        content.GetProperty("sections").EnumerateArray().Select(item =>
            (item.GetProperty("id").GetString()!, item.GetProperty("title").GetString()!));
}

internal sealed class InstallerState : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;
    public InstallerContract Contract { get; }
    public IReadOnlyList<Device> Devices { get; private set; } = [];
    public IpaReport? Ipa { get; private set; }
    public string? ArchivePath { get; private set; }
    public string? LogPath { get; private set; }
    public string Log { get; private set; } = "";
    public string? Error { get; private set; }
    public bool Busy { get; private set; }
    public bool Cancelling { get; private set; }
    public bool Installed { get; private set; }
    public string Phase { get; private set; } = "";
    private string selectedDevice = "";
    public string SelectedDevice
    {
        get => selectedDevice;
        set { selectedDevice = value ?? ""; Changed(); }
    }
    public Device? CurrentDevice => Devices.FirstOrDefault(device => device.Id == SelectedDevice);
    public bool ProfileMatches => Ipa is not null && CurrentDevice is not null
        && Ipa.Devices.Contains(CurrentDevice.Udid)
        && Version.TryParse(NormalizeVersion(CurrentDevice.Os), out var deviceVersion)
        && Version.TryParse(NormalizeVersion(Ipa.MinimumOs), out var minimumVersion)
        && deviceVersion.Major >= 17 && deviceVersion >= minimumVersion
        && DateTimeOffset.TryParse(Ipa.Expires, out var expiry) && expiry > DateTimeOffset.UtcNow;
    public bool CanInstall => !Busy && ProfileMatches && ArchivePath is not null && Error is null;
    public string Status => Busy ? Phase : Error ?? (Installed ? Contract.Text("installed")
        : Ipa is null ? Contract.Text("importNeeded")
        : CurrentDevice is null ? Contract.Text("deviceNeeded")
        : !ProfileMatches ? Contract.Text("profileMismatch") : Contract.Text("ready"));
    public InstallerState(InstallerContract contract) => Contract = contract;
    private static string NormalizeVersion(string value) => value.Contains('.') ? value : value + ".0";

    public void Begin(string phase)
    {
        Busy = true;
        Cancelling = false;
        Installed = false;
        Error = null;
        LogPath = null;
        Phase = phase;
        AppendLog("\n" + phase);
        Changed();
    }

    public void Fail(string error) { Error = error; AppendLog(error); Changed(); }
    public void Finish() { Busy = false; Cancelling = false; Changed(); }
    public void Cancel() { Cancelling = true; Phase = Contract.Text("cancelling"); Changed(); }
    public void CommitArchive(string path) { ArchivePath = path; Changed(); }
    public void AppendLog(string message)
    {
        Log += message + "\n";
        if (Log.Length > 128 * 1024) Log = Log[^(96 * 1024)..];
        Changed(nameof(Log));
    }

    public void Receive(JsonElement item)
    {
        switch (item.GetProperty("type").GetString())
        {
            case "session": LogPath = item.GetProperty("log_path").GetString(); break;
            case "log": AppendLog(item.GetProperty("message").GetString() ?? ""); break;
            case "progress":
                if (!Cancelling) Phase = item.GetProperty("message").GetString() ?? Contract.Text("working");
                break;
            case "error": Fail(item.GetProperty("message").GetString() ?? "Device operation failed."); break;
            case "cancelled": Fail(Contract.Text("cancelled")); break;
            case "result":
                if (item.TryGetProperty("devices", out var devices))
                {
                    Devices = devices.EnumerateArray().Select(device => new Device(
                        device.GetProperty("id").GetString()!, device.GetProperty("udid").GetString()!,
                        device.GetProperty("name").GetString()!, device.GetProperty("os").GetString()!)).ToArray();
                    if (!Devices.Any(device => device.Id == SelectedDevice))
                        selectedDevice = Devices.Count == 1 ? Devices[0].Id : "";
                }
                if (item.TryGetProperty("ipa", out var ipa))
                    Ipa = new IpaReport(ipa.GetProperty("bundle").GetString()!, ipa.GetProperty("team").GetString()!,
                        ipa.GetProperty("expires").GetString()!,
                        ipa.GetProperty("devices").EnumerateArray().Select(device => device.GetString()!).ToArray(),
                        ipa.GetProperty("minimum_os").GetString()!, ipa.GetProperty("bytes").GetInt64());
                if (item.TryGetProperty("installed", out var installed)) Installed = installed.GetBoolean();
                break;
            default: throw new InvalidDataException("Unsupported installer response.");
        }
        Changed();
    }

    private void Changed([CallerMemberName] string? property = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(property == nameof(Log) ? property : ""));
}
