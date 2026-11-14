using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Threading;

namespace Nickel.Desktop;

// Discord Rich Presence for the launcher, over Discord's local pipe (\\.\pipe\discord-ipc-0..9): handshake
// (op 0, v 1, client_id) then SET_ACTIVITY (op 1) with the launcher's process id. Same application as the
// in-game Discord Presence module. While Nickel is loaded in Minecraft the launcher clears its own activity and
// the game's module shows instead. Without Discord running nothing happens; the connection is retried.
public static class LauncherPresence
{
    private const string ApplicationId = "1569624154066387026";
    private const string Icon = "https://cdn.discordapp.com/app-icons/1569624154066387026/f2767b7d518e909a6356a7b17c14bf66.png";
    private static readonly object Guard = new();
    private static bool enabled;
    private static string? details = "In the launcher";
    private static readonly long Start = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
    private static Thread? worker;
    private static readonly AutoResetEvent Wake = new(false);

    public static void SetEnabled(bool on)
    {
        lock (Guard)
        {
            enabled = on;
            if (on && worker == null) { worker = new Thread(Run) { IsBackground = true, Name = "Discord presence" }; worker.Start(); }
        }
        Wake.Set();
    }

    // null clears the launcher's activity (Nickel loaded: the game shows its own).
    public static void SetDetails(string? text)
    {
        lock (Guard) { if (details == text) return; details = text; }
        Wake.Set();
    }

    private static void Run()
    {
        NamedPipeClientStream? pipe = null; string? sent = "\u0000";
        while (true)
        {
            bool on; string? want;
            lock (Guard) { on = enabled; want = details; }
            try
            {
                if (!on || want == null)
                {
                    if (pipe != null && sent != null) { Write(pipe, 1, Activity(null)); sent = null; }
                    if (!on && pipe != null) { pipe.Dispose(); pipe = null; sent = "\u0000"; }
                }
                else
                {
                    if (pipe == null) { pipe = Connect(); sent = "\u0000"; }
                    if (pipe != null && sent != want) { Write(pipe, 1, Activity(want)); sent = want; }
                }
                if (pipe != null) Drain(pipe);
            }
            catch { pipe?.Dispose(); pipe = null; sent = "\u0000"; }
            Wake.WaitOne(pipe == null && on ? 15000 : 5000);
        }
    }

    private static NamedPipeClientStream? Connect()
    {
        for (var i = 0; i < 10; ++i)
        {
            var pipe = new NamedPipeClientStream(".", "discord-ipc-" + i, PipeDirection.InOut, PipeOptions.Asynchronous);
            try { pipe.Connect(200); } catch { pipe.Dispose(); continue; }
            Write(pipe, 0, JsonSerializer.Serialize(new { v = 1, client_id = ApplicationId }));
            return pipe;
        }
        return null;
    }

    private static string Activity(string? text)
    {
        object args = text == null
            ? new { pid = Environment.ProcessId }
            : new
            {
                pid = Environment.ProcessId,
                activity = new
                {
                    details = text,
                    timestamps = new { start = Start },
                    assets = new { large_image = Icon, large_text = "Nickel" },
                    buttons = new[]
                    {
                        new { label = "Website", url = MainWindow.WebsiteUrl },
                        new { label = "Discord", url = MainWindow.DiscordUrl },
                    },
                },
            };
        return JsonSerializer.Serialize(new { cmd = "SET_ACTIVITY", args, nonce = Guid.NewGuid().ToString() });
    }

    private static void Write(Stream pipe, int op, string json)
    {
        var body = Encoding.UTF8.GetBytes(json);
        var frame = new byte[8 + body.Length];
        BitConverter.GetBytes(op).CopyTo(frame, 0); BitConverter.GetBytes(body.Length).CopyTo(frame, 4); body.CopyTo(frame, 8);
        pipe.Write(frame, 0, frame.Length); pipe.Flush();
    }

    // Reads and discards Discord's replies so its side of the pipe never fills up (one read in flight at a time).
    private static Task<int>? pending;
    private static readonly byte[] Buffer = new byte[4096];
    private static void Drain(NamedPipeClientStream pipe)
    {
        while (true)
        {
            pending ??= pipe.ReadAsync(Buffer, 0, Buffer.Length);
            if (!pending.Wait(30)) return;
            var read = pending.Result; pending = null;
            if (read <= 0) throw new IOException("Discord closed the pipe");
        }
    }
}
