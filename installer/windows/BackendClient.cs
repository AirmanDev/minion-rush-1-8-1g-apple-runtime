using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;

namespace MinionRushInstaller;

internal sealed class BackendClient(string? executable = null, IEnumerable<string>? prefix = null)
{
    public async Task Run(string workspace, IEnumerable<string> arguments,
        Func<JsonElement, Task> receive, CancellationToken cancellation)
    {
        var start = new ProcessStartInfo(executable ?? Path.Combine(AppContext.BaseDirectory, "backend", "MinionRushDeviceBackend.exe"))
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
            WorkingDirectory = AppContext.BaseDirectory
        };
        foreach (var argument in prefix ?? []) start.ArgumentList.Add(argument);
        start.ArgumentList.Add("--workspace");
        start.ArgumentList.Add(workspace);
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        using var process = Process.Start(start) ?? throw new IOException("The device backend could not start.");
        var stderr = process.StandardError.ReadToEndAsync();
        bool result = false;
        var reading = ReadEvents();
        try
        {
            await reading.WaitAsync(cancellation);
            await process.WaitForExitAsync(cancellation);
            string detail = await stderr;
            if (process.ExitCode != 0 || !result)
                throw new IOException(string.IsNullOrWhiteSpace(detail)
                    ? "Device operation failed. Review the activity log." : detail.Trim());
        }
        catch
        {
            if (!process.HasExited)
            {
                try
                {
                    await process.StandardInput.WriteLineAsync("cancel");
                    await process.StandardInput.FlushAsync();
                    await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(10));
                }
                catch (Exception error) when (error is IOException or TimeoutException or InvalidOperationException)
                {
                    if (!process.HasExited) process.Kill(entireProcessTree: true);
                    await process.WaitForExitAsync();
                }
            }
            try { await reading; } catch (Exception) { }
            throw;
        }

        async Task ReadEvents()
        {
            while (await process.StandardOutput.ReadLineAsync() is { } line)
            {
                if (line.Length == 0) continue;
                if (line.Length > 1024 * 1024) throw new InvalidDataException("Oversized backend response.");
                using var document = JsonDocument.Parse(line);
                var item = document.RootElement;
                if (item.GetProperty("protocol").GetInt32() != 1)
                    throw new InvalidDataException("Unsupported backend protocol.");
                if (item.GetProperty("type").GetString() == "result") result = true;
                await receive(item.Clone());
            }
        }
    }
}
